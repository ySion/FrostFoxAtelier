# 文件夹工程与资源浏览

工程的入口是目录，`Config/Project.json` 是识别标记。创建工程要求目标目录为空；打开工程不转换任意已有目录。命令行和 File 菜单使用同一 Project、Session 与应用执行链。

```text
MyGame/
  Config/Project.json
  Content/
    Scenes/Main.scene
    Meshes/                 # 首次导入时创建
  Source/                   # 自有语言源码
  Cache/                    # 可重建，不保存作品的权威数据
  .gitignore                # 忽略 /Cache/
```

## 使用

- File → New Project Folder / Open Project Folder 选择目录；也可运行 `atelier_editor --new-project <目录>` 或 `atelier_editor --project <目录>`，支持中文路径。
- Ctrl+S 保存当前工作区域：Code 区保存当前源码；SOP 程序化图上下文中保存活动 `.procgraph` 资产；资源检查或 Parameters 中获得编辑焦点的具名材质保存该资产；场景字段和场景 MI 保存场景。File → Save Scene 始终保存场景；Save All 保存源码、脏材质、脏 `.procgraph` 与场景。Ctrl+Shift+S 另存场景到当前工程 Content 内，File → Open Scene 打开同一工程中的场景。标题显示工程、场景和未保存标记。
- 关闭窗口、打开其他工程或场景前处理未保存修改。保存决定完成后才读取目标场景，取消新建也不会创建目录。加载在独立、停止的新 Session/World 中完成；失败保留当前文档，成功才撤下旧 UI 绑定和文档。
- Config 的 `startup_scene` 是打开工程时加载的场景，路径相对工程根。另存场景不隐式修改启动场景配置。
- Source 文件由工程统一采集、编译并绑定到真实场景类型。Project 菜单提供创建组件源码、构建、重建类型、运行、停止和项目 UI 预览。

## 异常退出时的场景保全

Application 捕获帧循环异常（包括 Renderer 返回的终态错误）后，在 Session 释放前尝试一次保全；帧循环外的异常栈展开由析构入口兜底。正常关闭沿用原来的保存/放弃流程。

未确认的参数或变换手势先取消，再使用同一个 `Session::serialize` 保存最后已提交的作者 Scene。输出为 `%LOCALAPPDATA%/FrostFoxAtelier/Recovery/<唯一标识>/Scene.scene`，日志以 `Scene recovery saved:` 报告完整路径。写入复用原子文件创建，不覆盖原场景、不标记文档已保存；重复失败不会重写快照或替换首次原因。

Scene 先写入并刷新到磁盘，随后 `recovery.json` 记录原工程、原场景、失败原因、时间、未确认手势与脏材质资源名称。侧记写出失败时仍保留已经写成的 Scene，并输出警告。序列化或磁盘失败明确输出 `Scene recovery failed:`，不宣称恢复成功。

这条路径不复制或解析资产，缺失 mesh 等外部资源不会阻止有效作者 Scene 的原始引用写出。它不是完整工程备份：独立材质资产、Source 文本草稿和没有持久化来源的程序化网格不在本次保全范围内；原 Scene 序列化本身拒绝的状态仍会失败。强制结束进程、访问违规或断电不能保证执行异常保全。

恢复时先保留原工程及快照，将 `Scene.scene` **复制为原工程 Content/Scenes 下的新文件名**，再通过同一工程的 Open Scene 验证类型和资源；确认后再决定保存位置。不要直接覆盖原文件。

Scene 恢复路径目前没有自动验收；真实设备失效后的 Application 调用链需在 GPU 故障风险消除后复验。

## 后续方向：可响应加载与多场景文档

以下为设计方向，尚未实现。当前 Application 只持有一个 Session、一条场景路径、一台 Camera 和一个已接受的视口状态；Open Scene 会同步读取文件、构建源码、反序列化对象及解析资源，再替换旧文档。停靠标签不等于独立场景，现有加载也不能保证大场景期间持续响应。

场景文档与视图应分开拥有状态：

- 工程管理目录、资源身份和 Source 文档。场景文档管理 Session/World、场景路径、未保存状态、撤销、选择及已绑定代码的寿命。
- Scene View 绑定一个稳定的文档身份，拥有自己的相机、显示设置、手势和接受帧。不同 Scene View 可以展示不同场景，同一场景也可被多个视图观察；打开场景不必先关闭当前场景。
- Scene Tree、Entity Structure、Parameters 和场景命令随当前文档切换。已开始的拖动、编辑、菜单命令和异步请求必须保留原文档身份及代次，不能因用户切换标签而落到另一场景。
- 同工程重复打开同一路径默认激活现有文档；额外观察角度通过新建视图表达。关闭视图与关闭文档分开，关闭最后一个视图时处理未保存修改和正在执行的加载。非活动文档保留数据，默认不持续渲染或自行运行游戏。

加载采用同一条阶段式流程：后台读取、解析与准备不可变资源，所有者线程按帧预算构建和连接对象，Renderer 按既有资源寿命与提交合同完成上传和首帧准备，候选就绪后发布。Runtime 当前是单线程拥有的上下文；不能直接将整个 `Session::deserialize` 丢入后台，并与主线程共用 Project 缓存或活动 Runtime。源码编译准备与 Runtime 类型绑定也需要明确分开。首帧 shader/pipeline 创建、纹理上传和旧 GPU 资源退休同样属于加载链，否则只把磁盘读取移到后台仍会在第一帧卡住。

加载中的目标视图显示阶段、进度和取消入口；其他已打开文档继续响应。进度依据实际文件、对象和资源工作量，未知工作量显示阶段而不伪造百分比。资源逐步到达可以先展示占位和已就绪部分，但未完成引用关系和类型绑定的文档不开放编辑、保存或运行。失败、取消及关闭目标只清理候选；晚到结果通过请求代次丢弃，旧场景与其修改保持有效。只共享已明确不可变的资源，材质草稿等可变作者数据仍遵守现有权威编辑入口。

建议先拆出现有加载过程的阶段与取消边界，让单场景打开保持响应；随后将文档拥有关系从 Application 移到文档集合，再接入多 Scene View。两步共用加载和发布入口。UI、资源浏览器与 Editor Control 最终都使用该入口，异步操作继续复用现有 job/request 身份；多文档目标需要显式文档身份，不能沿用“当前场景”来解释晚到请求。

### 加载与视图的发布规则

文档身份独立于磁盘路径、Entity 身份与停靠标签；未保存的新场景也有稳定身份。每次加载另有请求代次，关闭、重新加载或换工程先撤销其发布资格。后台任务只持有拥有型输入、取消标记及候选结果，不借用活动 Session、UI 控件、Project 可变缓存或原生窗口。候选发布前再次验证工程、目标文档及请求代次。取消后仍在完成的磁盘操作可以结束，但其结果不得重新打开标签或覆盖当前文档。

第一步保留现有 Session 的独立 Runtime/World 寿命，同一文档的多个视图共享这一套对象；不为了后台加载将运行中的 Runtime 迁入工作线程。源码准备、对象构建、资源上传分别提供明确的暂停/继续边界，循环中按阶段计数或时间预算让出主循环。后台完成事件只通知所有者，UI、Runtime 和 GPU 发布仍在对应的安全阶段执行。加载不通过在反序列化中重入 Windows 消息循环来维持假响应。

场景状态分为准备中、可预览、可编辑、失败/取消。可预览阶段可以显示占位和已就绪的资源；只有对象身份、跨对象引用和源码绑定完整后才开放编辑。旧场景一直保持可用，替换式重载只有成功发布后才退休旧文档。GPU 资源按现有 fence 机制退休，不能在取消或切换标签时同步等待整个设备空闲。

多 Scene View 也要求 Renderer 接受多个视图：同一原生窗口只组合一次 UI 并 Present 一次；同场景的网格/材质可共享，而相机、前帧矩阵、AA 历史、拾取深度和接受帧属于具体 View。对象缓存键必须包含场景身份/代次，不能把两个 Runtime 中数值相同的 ObjectId 当成同一个对象。先接入这个视图合同，再开放重复 Scene View 标签；仅复制当前固定控件 key 或给同一 HWND 创建多个交换链都不能实现正确的多视图。

### 撤销栈归属与路由

| 修改对象 | 历史拥有者 | 多视图行为 |
| --- | --- | --- |
| Entity、组件、场景属性、实例材质覆盖 | 场景文档的 Session | 同场景所有 View 共用；不同场景隔离 |
| 共享 Material 等资源的作者属性 | 工程内该资产的文档，按资源 GUID 唯一 | 已打开场景消费同一已发布资产版本 |
| Source/文本文件 | 该文件的源码文档 | 多个编辑视图共享文本历史 |
| 尚未提交的字段文本、IME 组合 | 当前编辑控件的本地草稿 | 沿既有文本编辑规则先处理草稿 |
| 相机、目录宽度、停靠和显示模式 | View/工作区状态 | 不写入场景或资产历史；导航记录另行管理 |

当前共享材质历史仍存放在 Session 内；接入多文档时必须先将资产作者状态与历史提升到工程的资产文档。同一共享材质不能在两个场景内各自保有可写草稿和撤销游标。各 Runtime 消费资产的已发布快照，资源参数更新通过既有类型与材质入口传播，不新增第二种反射类型或跨 Runtime 对象指针。

Ctrl+Z/Ctrl+Shift+Z 先服从当前文本控件的草稿规则，再路由到获得编辑焦点的文档；菜单打开时固定目标，执行时验证该文档仍有效。菜单显示动作名称及必要的文档名。栈为空时保持禁用，不跳到“最近修改的其他场景”去撤销。Parameters 即使展示固定目标，也必须携带目标文档身份；不能用当前 Scene View 猜测属性所属文档。

一次拖动、数值编辑、多选删除或复制加移动对应一笔事务。开始时固定文档身份、对象代次、字段路径及原值；预览不追加历史，提交恰好追加一次，取消恢复原值。切换文档或关闭目标先取消活动手势，晚到的松键不得提交到新文档。回放前验证全部目标及类型合同；失败不推进游标，不保留部分应用。沿用现有正式业务入口与 World 事务，不用通用回调闭包捕获控件、临时对象或当前选择来实现历史。

打开、加载进度、选择场景标签不进入编辑历史。异步导入/创建只有在目标文档仍允许编辑并成功发布时，才记为一笔原子操作；加载期间的局部候选不成为可撤销操作。撤销不重新读文件，重做使用该笔操作保留的不可变资源与对象身份；磁盘资源的后来变更不能悄悄改变同一笔重做的结果。首次跨场景功能优先支持复制：仅向目标场景追加一笔事务，源场景不变；跨场景移动需联合事务与两边历史的一致回放，在该合同完成前不开放。

保存点是历史中的状态标记，不是“当前操作数等于上次操作数”。保存后撤销应变脏，重做到保存点恢复干净；撤销后分叉会截断原 redo 分支，保存点若在被截断区间则失效。历史裁剪沿用这一规则，同时将当前每文档 128 条上限扩展为条数与保留内存的双重预算；共享网格/贴图按引用保有，避免每笔事务复制整份场景资源。关闭文档释放历史根，World、代码与 Runtime 仍按已有寿命顺序退出。

兼容 JIT 热更保留文档历史。字段布局或类型身份不兼容的重建属于文档代次切换：先完整准备候选，失败保留旧文档；成功后明确处理该文档的旧历史，不能将旧字段路径回放到新布局。Play 使用运行副本，模拟中的变化默认不写编辑历史；未来若提供“应用运行时变化”，需作为显式、可撤销的编辑事务。

实施时的端到端门槛包括：加载大场景期间持续处理输入和取消；关闭后迟到结果不复活；A/B 场景交替编辑只撤销目标场景；同场景多 View 共用一次历史；共享材质跨场景一致；保存/撤销/重做/分叉/裁剪的 dirty 标记正确；兼容热更保留历史，失败或不兼容重建不损坏旧状态。当前本节仍为设计，不能以已有单场景验收替代这些结果。

## 资源读取与作者写入

`assets::Reader` 是同步读侧，解析精确的内建网格与安装材质/Source，不需要 Project、Runtime 对象或设备。工程的 mesh、texture、material、sky、导入说明与依赖文件读取也由它负责；OBJ/glTF/WIC、材质与共用 JSON 编解码位于 `modules/engine/assets`。Reader 不创建目录、身份文件或作者材质文档，不持有 Runtime 根；读取 MS 作者数据和 Source 文本不编译程序。

资产值类型直接使用 `assets::AssetKind`、`assets::MeshAsset` 等声明；文件读取/UTF-8 转换、JSON 与材质 codec 分别通过 `assets/file.hpp`、`assets/json_format.hpp`、`assets/material_format.hpp` 使用，不经 `editor` 别名或转发头。`editor/assets.hpp` 仅声明编辑器部署目录定位 `asset_directory()`，不再作为资产读取入口。接口和格式升级遵循[当前开发基线](../../engineering.md#当前开发基线)。

Project 保留配置、目录作者扫描、sidecar 身份处理以及导入、保存、重命名事务；扫描完整候选通过校验后，发布给它组合的唯一 Reader。目录条目、键索引、revision、网格/天空/贴图内容缓存及图片弱缓存只由 Reader 拥有，Project 的目录接口只是只读视图；类型化读取直接使用 `project.assets()`，不另设 Project 读实现。刷新仍按现有文件大小/修改时间失效，导入预载复用同一图片缓存，失败时移除本次预载与本次写出的文件。借用的目录/说明视图不能跨发布保留；已返回的不可变内容拥有独立寿命。Reader 的调用与发布仍由所属线程串行执行，不提供异步或并发读取合同。

Runtime 中的 Texture/MS/MI 根和可变作者历史由 Session/宿主持有。当前 Scene v10、材质 v2；语言 `engine_sources` 使用独立摘要。Repro 的类型化依赖闭包来自 Session 冻结快照；Reader 只补充网格原始伴随文件，不另行递归读取磁盘材质关系。

### 引擎依赖摘要

`Reader::engine_dependency(key, expected_kind)` 查询本安装精确生产者，返回拥有型 `EngineDependency`（key、kind、semantic_version、digest_version、content_sha256）；不回退工程目录。内建网格由 `assets/builtin.cpp` 的生产者表维护，内建材质由 `assets/builtin_materials.cpp` 提供；材质摘要 v2 包含真实 MS 作者数据及 Source 文本，使用当前确定性 JSON writer 编码。语义版本与摘要编码版本独立演进；摘要根据实际发布内容计算，不认证未来 C++/shader 行为。无 Project/Runtime/设备即可读取，安装不产生可写目录或材质文档。

`mesh_content_sha256` 的编码 v2 使用 `Atelier.Mesh` 域字节、u32 编码版本；整数小端，长度/数量为 u64，bool 为 u8，有限 float 为保留正负零的 IEEE-754 u32 位。字符串是长度前缀的原 UTF-8 字节。不哈希 padding、指针或容器布局。64 KiB 有界缓冲批量送入共用 `foundation::Sha256`，只在依赖核对冷路径计算。

字段顺序为：顶点 position/normal/uv/tangent；bounds；每三角形有效边 mask（省略边数组等同全 7）；有序 part 的 first_vertex/vertex_count/material；有序槽名；按槽顺序编码默认关系的有无及目标 key。先拒绝重复、歧义、不存在槽的默认关系，不用覆盖或排序掩盖关系。网格不再内嵌 Surface，材质作者值和图片不参与网格摘要；修改材质内容不改变几何摘要，修改默认绑定目标则会改变。资产自身 key/显示名不是内容，默认关系目标 key 是内容。

工程 mesh 与引擎 mesh 使用同一编码。repro 的必需版本字段、实际集合核对与旧包重导出见[复现工程](control.md#复现工程)。引擎语言 Source 仍使用独立的既有摘要合同。

## 资源浏览器

原 Asset Shelf 沿原停靠视图演进为 Asset Browser。左侧常驻目录树以 Project、Engine 为并列根：Project 展示实际 Content、Source、Config 目录，Engine 展示只读内置网格；未打开工程时 Project 根禁用。路径与搜索位于同一紧凑工具行，路径菜单可访问祖先目录，返回和上级操作使用同一导航状态。类型筛选、List/Details/Tiles 和资源操作各由图标菜单承载。

目录树与结果区之间使用 DSL Splitter，可拖动调整目录宽度；两边分别保留至少 96/64 DIP。Esc、失焦、捕获丢失或关闭正在编辑的区域会取消本次调整；临时缩窄整个面板只限制显示宽度，不抹掉用户此前选择的宽度。目录宽度沿现有工作区状态保留，不进入场景撤销，不触发 viewport 渲染目标缩放。

单击资源改变浏览器与参数检查目标，不修改 Scene Tree 选择或创建 Entity。Material 资源打开共享参数编辑，`.procgraph` 程序化图资源显示元数据并通过双击/Enter 或 Open 进入 SOP 视口与 Network 编辑上下文，其他资源显示元数据。更多菜单及资源右键菜单中的 Open 进入目录、打开场景或进入 `.procgraph` SOP 上下文，Details 的双击/Enter 走同一 Open 入口。Add to Scene 执行既有 StaticMeshEntity 创建命令；New Procedural Graph 创建 `Content/**/*.procgraph`（JSON v3）及其 `.asset.json` GUID 身份文件；Import Mesh、Refresh 也沿原命令入口。右键先选中当前资源，再构建菜单；原资源消失或选择改变后关闭旧菜单。切换视图保留所选资源、过滤、排序和列宽。Tiles 使用类型图标，不声称已经实现模型缩略图。

查询或类型过滤排除当前资源时立即清空选择，并禁用对应 Open/Add；仍匹配的选择保留。清除过滤不恢复先前的隐藏选择。目录导航使用共享 Tree 的展开、选择与键盘行为；从路径或内容进入目录时显露对应树行。视图和类型菜单使用互斥单选标记。目录刷新淘汰失效导航记录，当前目录消失时回到有效来源根。

Details 使用共享 Table 的 Name / Type / Size 列，列头显示排序方向；Size 按原始字节数排序、右对齐，目录始终排在文件前。列边界拖动有最小宽度，松手保存本次列宽，Esc/失焦/捕获丢失取消。窄面板保留各列并提供横向滚动。整表是一个 Tab 停靠点，方向键、Home/End/PageUp/PageDown 移动当前单元格并显露所选行。排序保持资源身份；选择变化复用原数据快照。详细控件合同见[共享表格](../ui/README.md#表格)。

工具行、目录树、内容槽和状态行由 `AssetBrowserShell` 声明，List/Details/Tiles 均由 `AssetResults` 的同一个 Table 节点声明，均位于 [editor.ui.source](../../../assets/editor.ui.source)。保存可更新这些区域的布局，同时保留兼容的控件身份、选择、排序列宽和搜索草稿。浏览器只提供目录/资源快照和既有命令；目录模板按实际变化缓存。全部结果由共享 Table 按当前视图虚拟绘制，原浏览器虚拟列表、合成资源按钮与逐项菜单选择路径已移除。

Import Mesh 导入 Wavefront OBJ 的位置、法线、UV、MTL 漫反射材质及依赖贴图。简单多边形（每面最多 64 个角点）按投影耳切三角化，保留原边界 mask；支持正/负索引，缺少法线时生成面法线，带贴图的面必须提供有效 UV。OBJ 的 V 轴转换到图片左上原点，超出 [0,1] 的 UV 保留并重复采样。单独 OBJ 保持 `Content/Meshes/<文件名>` 的位置；带依赖的模型使用 `Content/Meshes/<模型名>/` 独立目录，保持相对依赖关系。材质通过普通 MI 与网格默认表表达，没有组件颜色乘色路径。

MTL 支持 `Kd`、`d`/`Tr`、`map_Kd` 与 `map_d`。WIC 解码 PNG/JPEG/BMP 等系统支持格式；颜色贴图按 sRGB 采样，遮罩按线性值读取。独立 Material 资源支持金属度/粗糙度、法线与 Y 轴约定、HDR 发光和 Mask，并由 Scene Lit 的阴影、反射与 DDGI 消费。OBJ/MTL 的具体映射、通道及着色范围见[材质系统](../renderer/materials.md)；本轮不包含高度贴图或半透明混合。带 `-s`/`-o` 等选项的 map 明确报错，动画仍未导入。

依赖只能位于 OBJ 所在目录及子目录；绝对路径、上级路径、逃逸链接、缺失文件与非法数据在发布前拒绝。单文件最多 128 MiB、整包最多 512 MiB、解码像素最多 768 MiB，图片单边最多 8192；最多 512 个依赖文件、1024 个命名材质、100 万个位置/法线/UV 和 100 万个三角形。导入验证和写盘使用相同字节快照，先检查现有目录；Import 不覆盖无关文件，Reimport 只替换已验证归属的内容。失败恢复已替换文件的内容和时间戳，删除本次新增输出及空目录，清理失败明确报告；不承诺跨文件断电原子性。

Import Mesh 也接受 glTF 2.0 的 `.gltf`，buffer 与图片为外部文件，图片须为 Project 可登记为贴图的 PNG、JPEG、BMP 或 TIFF 文件；GLB、data URI 和存放在 buffer 里的图片不支持。默认场景中带 mesh 的节点按文件顺序深度优先展开，节点 TRS 或矩阵烘焙进同一个 Mesh，每个 primitive 一个材质分段；同一 mesh 被多个节点引用时各自展开。glTF 是右手系，引擎是左手系，两者都以 +Y 向上、以米为单位（[renderer 合同](../renderer/contract.md#场景和-ui)），所以导入时镜像 X 轴：位置和切线经矩阵变换，法线经余因子矩阵，作者切线的 w 在行列式为负时取反。缺少 NORMAL 时按 glTF 正面生成面法线；缺少 TANGENT 时由 MikkTSpace 生成，并统一到 glTF 法线贴图 +Y 指向 V 减小方向的约定。UV 不翻转。

glTF 材质映射：baseColorFactor 乘 sRGB 的 baseColorTexture；metallicRoughnessTexture 的 G 通道作粗糙度、B 通道作金属度，两个因子缺省为 1；normalTexture 及其 scale；emissiveFactor 乘 emissiveTexture；MASK 与 alphaCutoff（缺省 0.5）；doubleSided 同时决定背面裁弃和单面发光，缺省为单面。采样器的 wrapS 与 wrapT 必须相同（Repeat、Clamp 或 Mirror），过滤方式由渲染器决定。BLEND 材质的 primitive 不导入，导入时在 Output 记一条说明；occlusion 贴图、TEXCOORD_1、COLOR_0、KHR_lights_punctual、相机和其他非必需扩展被忽略。extensionsRequired 非空、非三角形列表、sparse 或 normalized accessor、蒙皮、贴图引用 texCoord 非 0、节点图不是树时明确报错。只读取被导入材质引用的图片。

场景按槽名绑定材质，所以 glTF 的槽名唯一：无名材质叫 `material <序号>`，重名的材质以及与无材质 primitive 所用的 `default` 重名时，后者依次加 ` (2)`、` (3)`。OBJ/glTF Import 为实际有作者材质的槽生成普通 v2 MI，放在模型的 `Materials` 目录；没有赋材质的槽保持无默认关联，使用 Default World Grid，不制造白色 MI。文件名中的非法字符、设备名及大小写重名按现有规则处理。图片分别得到稳定身份，内容相同仍可共享解码像素。网格 `.asset.json` 的 `materials` 保存槽名到材质键，作为[网格默认材质](#材质与贴图资源)。全部身份在写盘前确定，完整候选验证成功后才发布。

glTF 上限：文档 16 MiB，buffer 合计 512 MiB，单个图片文件 64 MiB，最多 256 张图片，解码像素合计 2 GiB，最多 800 万个三角形、65536 个节点。依赖规则与 OBJ 相同：URI 百分号解码后必须是源目录内的相对路径。上限按官方 Sponza 的 2K 数据留约一倍余量：其中最大的主体文件约 375 万个三角形、134 MiB buffer、72 张图、1.1 GiB 解码像素。图片在最多 8 个线程（不超过逻辑核数的一半）上并行解码；任一张失败时停止其余解码，报告第一个错误。

glTF 由 core/json 的专用政策入口解析：UTF-8 严格、允许 BOM、不另设嵌套深度限制，重复的解码后成员键沿用后值覆盖。这遵循 [glTF 2.0 JSON Encoding 与整数字段规则](https://github.com/KhronosGroup/glTF/blob/9ec408bd03f6c622a026a8989d24257a83311835/specification/2.0/Specification.adoc)（KhronosGroup/glTF@9ec408bd03f6c622a026a8989d24257a83311835）：键唯一与后值覆盖均为 SHOULD。索引、计数等整数字段接受有限、非负、无小数且在目标范围内的数值，包括 `100.0`、`1e2`，不再截断小数。OBJ/MTL 自有格式、glTF 格式/schema、未知网格扩展名、图片解码及像素预算拒绝返回 `invalid_storage`，保留原错误文本。批解码在全部 worker join 后返回第一个已记录的错误，不返回部分图片；线程启动失败仍致命，不降级。失败时缓存可留下弱引用，既有外部像素所有者不受影响。

OBJ/glTF 尾部调用 `geometry::Mesh::create`；空 OBJ、零法线及无效作者切线等构造拒绝映射为 `invalid_storage`，保留原诊断，不以消费侧扫描或 catch 桥接复制几何规则。`material::Image::create` 也通过 Result 返回尺寸/RGBA 存储拒绝；WIC 解码前的尺寸与预算预检仍保留。工厂失败不发布候选，Reader 与 Project 的其他异常链不由这次迁移清零。

`generate_tangents` 的调用前提是有限位置/UV、单位法线、非空且顶点数不超过 `INT_MAX` 的三角形列表。Mesh 工厂验证几何；glTF 在展开前限制总顶点数不超过 2400 万，随后按 primitive 调用，不重复扫描。固定回调和有效输入下 MikkTSpace 返回失败走 CPU 分配 fatal；其内部可空 malloc 失败转慢路径仍是第三方分配适配残留。

属性类型按 schema 检查，`null` 不等同于缺省：`images`、`materials`、`extensionsRequired` 必须是数组，消费到的 `pbrMetallicRoughness` 必须是对象。类型错误返回带 JSON 路径的 `invalid_storage`；本片不增加 minItems 检查，空数组仍按既有消费规则处理。

Refresh/F5 重新扫描目录，不在 UI 每帧访问磁盘，也不自动向已加载的 Session 导入新版本。普通磁盘缓存按大小/修改时间失效，未变化刷新保留 revision 和资源；显式发布的精确网格快照跨目录刷新保留，不退回较新的磁盘内容。图片按 SHA-256 共享不可变解码内容，弱缓存不额外延长像素寿命。时间戳未变的外部改写不保证由普通 Refresh 发现，手动 Reimport 则读取实际内容。

手动 Mesh Reimport 保持网格身份，按唯一同名槽保留既有 MI 文件及用户编辑；仅新作者槽创建 MI，不推断重命名或合并源材质参数。Session 先发布本次图片，再解析新材质，最后发布几何和默认表；已打开的未保存 MI 不被覆盖。暂时消失的显式槽绑定保留为休眠作者数据，同名槽回来时恢复。Live 传输精确几何快照，不要求接收端重读磁盘。导入不进入 Undo，也没有文件系统自动导入器。


每个 OBJ 旁的 `<文件名>.asset.json` 保存版本和持久 UUID；网格的元数据还可以有 `materials`（槽名到材质键），每个键必须对应网格中唯一的槽，否则网格无法加载。移动、重命名资源时须一同移动该元数据，并保持 MTL/贴图的相对引用；复制资源应使用 Import，以获得独立身份。Refresh 为新增 OBJ 建立元数据，重复 ID 或非法元数据明确报错。导入或新建的资源及其元数据共同位于 Content，需要与源文件一同保存到版本控制；Cache 不承担资源身份。目录索引没有固定项数上限；`.asset.json` 和临时文件不单列于目录索引，原文件保留。查询结果按目录版本缓存；List/Details/Tiles 共用 Table 的可见单元格绘制，不为整个目录建立布局节点。外部调用方可沿 [Control 资产分页](control.md#资产目录分页) 读取同一目录，响应及 repro/Source 的独立限额不变。

## 材质与贴图资源

New Material 创建 MI `.material`；New Material Source 创建 MS `.ms.material` 与 Source。Import Texture 导入 PNG/JPEG/BMP/TIFF 到 `Content/Textures`。这些资源与 Mesh 共用 Project 索引及 `.asset.json` GUID。材质只读写 v2；MS 保存源码路径、策略与具名输入，MI 保存父键与稳定声明路径的类型化稀疏 entries，完整合同见[材质系统](../renderer/materials.md)。

Parameters 从实际 MS 声明生成任意数量的数值、向量、颜色和纹理控件，不反射固定 PBR 参数结构。编辑沿 Session 的 begin/preview/commit/cancel 链；历史只保存触及路径的作者项及编辑对象根，不保存图片或父版本。关闭覆盖保留本地值，首次预览取消恢复原先缺项状态。失配、已删除声明和休眠项保留并诊断，未启用或失配纹理不加载为执行依赖。

每个材质资产拥有独立历史与保存点，场景实例进入场景历史；Save Scene 不隐式保存材质资产草稿。Parameters 内联材质的 Undo/Redo/Ctrl+S 按当前控件绑定的作者身份路由，不按父材质或最近修改目标猜测；场景 MI 即使继承具名 MI 也只进入场景历史。选择或焦点切回场景后不沿用旧材质目标；材质历史为空时禁用 Undo/Redo，不回退到场景历史。Save All 保存材质、源码和场景。运行与重建类型先提交并保存材质草稿；关闭、切换文档处理未保存资产，Control 替换文档拒绝未保存资产。内置 MS/MI 作者只读，可建立子 MI；MS 文本经源码编辑器显式保存发布。

Entity/Component 按网格槽提供共享材质指派、Edit Shared Material、Use Mesh Default 和 Create Scene Material Instance。每槽依次取显式绑定、网格默认关联、Default World Grid；没有组件颜色或固定参数回退。新场景 MI 继承当前材质，无绑定时继承 Grid；MS→MI→MI 父链支持数值及纹理覆盖。复制 Entity 复制其可变场景 MI、保留复制范围内的共享关系，并继续共享命名资产祖先。

资源引用由 Runtime 强引用与 GC 管理；Reader 保留不可变图片缓存及持久索引。渲染消费自持程序、参数、图片的 ProgramSurface，不访问 Runtime 或磁盘；未变内容复用，旧捕获保持自身版本直到消费者退休。完整组的 GPU 准备、采用和接受帧拾取见[材质系统](../renderer/materials.md#gpu-批次与实际显示)。

外部图片改写不会自动更新已加载 Texture。手动 Texture Reimport 保持身份，发布新像素并通过 Live 转发；参数 Undo 不恢复旧图片。显式空纹理键使用 MS 类型化空值，单项缺失继承父内容或输入默认；定义或父链不可消费时使用错误材质，诊断与原作者值保留。

Scene 统一写出 v10：顶层 `materials` 按文档 ID 保存具名资产引用或场景 MI 作者数据，网格槽引用同一 ID。完整格式见[项目场景](../world/project-scenes.md#scene-v10)。Control Repro 冻结实际可达父链、精确 MS 文本和活动纹理像素，在独立副本中验证，不保存原工程；休眠 entries 保留作者值但不扩大执行依赖闭包。

## 持久格式与责任

本节记录现行格式与加载行为；已裁定的跨模块规则只见[持久化格式规则](../../persistence.md)。下述额外宽容、占位与恢复行为尚待产品审查，本次规则同步没有修改这些实现。

- Project 和资源身份文件为 UTF-8 JSON、`version: 1`；Scene 只读写 `version: 10`，其他版本拒绝。JSON 解析由锁定的 `core/json` 提供；glTF 使用上述专用政策。共用内部格式校验：16 MiB 上限、最多 32 层容器、禁止重复字段、身份及版本必须是非负整数、向量必须精确匹配长度且各项有限。float 字段写成能读回同一 float 的十进制，通常是最短形式（`0.2f` 写作 `0.2`，不写成扩为 double 后的 `0.20000000298023224`），double 字段写成 double 的最短表示；Scene 与材质文件共用这一写法。Project 与资源身份文件拒绝未知字段；Scene 的未知字段与不符值按下述加载策略处理。内部 JSON 结构不穿过 Project/Session 公共接口。
- Project 保存名称、相对 `startup_scene` 和可选的 `ui_source`（Source 内的 `.ui.source` 入口）。目录解析检查绝对路径、上级路径和解析后逃出工程的链接，不以进程当前目录寻找资源。
- Scene v10 保存有序 roots 与 Entity records、真实类型、名称、enabled、有序 children/components、共用的字符串 uint64 文档 ID、可选内联 space、材质和资源键。组件不保存空间 root/parent/TRS。存档不写 Runtime 的 TypeId/ObjectId/FieldId、内存地址或 GPU 资源。
- World 光照、效果与天光是 Environment Entity 上 `world.EnvironmentComponent` 的普通属性，随该组件保存；字段与 UI / Control 共用的 author-option 目录一一对应，包括 `direct_candidates`。没有 Environment 时使用默认值，文件中不生成它。`World::environment()` 是权威值；`Session::set_environment` 是 World Settings 与 Control 的唯一编辑入口，比对候选中的 World 选项，把变化的项写成一次组写入，记一条普通属性编辑历史；没有 Environment 时同一历史项先创建它，撤销即回到没有 Environment 的状态。无变化为空操作；运行中或关闭的 World 拒绝；Environment 存在但停用（组件或任一祖先停用）时也拒绝，原因提示先在 Parameters 里启用它，因为此时 World Settings 显示的是默认值。天空贴图不是 World 选项，由 `Session::set_sky_map(SkyAsset)` 单独设置：资产来自 `assets::Reader::sky_map`，空资产表示清除；同样记一条历史，没有 Environment 时在同一历史项里创建它，停用时拒绝，无变化为空操作。撤销和重做恢复历史里保存的那个指针，不重新解析键。`sky_rotation` 是普通属性。视口诊断从不进入 Scene。
- 编辑器渲染快照组合 World authoring 与视口偏好；Project Run 从载入的 World 读取同一配置，并复用 `SceneSnapshot` 投影真实材质与灯光；天光由宿主按 Environment 求出 `Frame.environment`，没有 Environment 或天光关闭时为零。探针滚动位置、历史、统计、可视化与编辑器观察相机不写入 Scene；场景 CameraComponent 和 Environment.main_camera 则是持久作者数据。
- TRS 按 `position` / `scale` 的 xyz 数组和 `rotation` 的 xyzw 数组保存，颜色同样是数值数组；不保存数学类型名或原生内存布局。统一到 `math.vector3` / `math.quaternion` 后，float 精度、分量顺序、局部变换与行向量组合语义保持一致。
- Scene 按限定类型名恢复已注册的 Entity/Component 派生类型；公开标量和内联记录进入 `properties`，新增未保存字段保留 Runtime 默认值。Physics Body 设置沿 `properties.body` 保存；普通引用为 null 或 `{"$ref":"ID"}`，序列为引用数组，持有/观察由 Runtime schema 决定。未知版本、失效关系、身份冲突和组合/空间环拒绝整个场景；未知类型、未知已保存属性、类型不符的值和缺失资源由宽容加载保留（占位对象、默认值加原值、占位资源、诊断），由严格加载拒绝；无法编码的字段保存失败；空网格保留为空。单场景最多 8192 Entity、每个 Entity 最多 1024 组件、文件最多 16 MiB。详见[项目场景合同](../world/project-scenes.md)。
- 加载策略分两种。宽容加载用于交互打开工程、场景和示例，以及 Rebuild Types：无法应用的数据按对象保留，诊断写入 Project Output（一行汇总加逐项列表）；Scene Tree 以琥珀色标出含保留数据的 Entity，Entity Structure 与 Parameters 把占位对象显示为 `类型 (missing type)`，Parameters 的 Saved Data 组只读列出保留值，Edit → Discard Saved Data 丢弃所选对象的保留值（可撤销）。缺失的 Material 或 Texture 在材质槽和贴图输入中显示为 `(missing) 键或名称`，目录中仍列出但无法读取的显示为 `(unreadable) …`；Mesh 不可用的组件多一行只读 `Mesh`，其材质绑定表列在 Saved Data；网格的默认材质或其贴图不可用时以替代品显示，诊断记在该组件的 `mesh` 字段。不可用按资源种类记录：场景把材质的键写成 Mesh 引用时，只有该 Mesh 不可用，材质照常打开、编辑和保存；贴图文件无法解码时，引用它的材质也照常保存。保存原样写回保留数据和不可用资源的引用，状态栏给出保留项数量；目标已删除的保留引用写为 null 并在 Project Output 报告。严格加载用于可复现流程：`ProjectOpenPolicy::require_scene`（`--profile-render-performance` 打开与 Control 复现导入）拒绝严格规则不允许的加载问题；info 诊断不触发拒绝，格式合法的稀疏材质声明失配保留作者项并使用继承值。Play 子进程宽容加载，开始时输出一行汇总；占位对象没有行为。Control 的 `editor.inspect` 对占位对象另报告 `placeholder: true` 与 `saved_type`，单对象详情列出 `saved_data`，网格组件带 `mesh.source` / `mesh.missing` 与 `mesh.default_materials`（每个默认槽的 `slot`、`object`、`source`、`missing`），材质槽带 `missing`。当前限制：文档级保留数据（未知的顶层键）只在 Project Output 报告，面板中没有对应显示。
- 物理角色、布娃娃/部件、力和运动学组件复用上述属性格式。DistanceConstraint 的 `properties.target` 保存统一文档观察引用，RagdollComponent.parts 保存显式 Entity 持有引用数组；全部对象创建后恢复引用并校验，场景外目标拒绝，不保存后端句柄。当前使用方式见 [Physic 示例](../../../examples/Content/Physic/README.md)。
- SceneQueryComponent 复用当前 Scene 格式保存 TraceSettings、局部端点和自动/忽略自身开关；枚举使用名称，运行中的查询请求与命中结果不写入文档。重新打开必须重新采样，详见[场景查询](../physics/scene-queries.md)。
- StaticMeshComponent 自身保存反射可见的 `mesh_source`；World 的 `set_mesh_binding` 一次替换来源、解析内容和不持久化的槽默认材质（编辑器按网格资产解析，`Session::mesh_defaults`），`read_static_mesh` 覆盖 Detached 与空网格。没有组件的消费者用 `Session::mesh_surfaces` 取同一份默认材质下每个槽的表面。裸 `set_geometry` 清空旧来源。编辑器不维护 Component ID 到资源键的权威副表。
- 重建先创建全部真实 Entity/Component，恢复值、资源与普通引用，再恢复有序组合和 Entity 空间参考，完整 forest 经统一批次注册；无效关系拒绝候选。加载 StaticMeshEntity 不重复调用带默认组成的快捷工厂。
- 保存先完成有效文本草稿，经同目录临时文件、FlushFileBuffers 和替换发布。写入失败保留旧文件；这不是跨文件事务或断电恢复保证。工程标记最后创建，未完成创建不会伪装成可打开工程。存档提交与目录刷新结果分开：成功保存后即更新保存点，资源刷新失败显示警告，不将成功写入误报为保存失败。
- 场景撤销回到保存位置后恢复干净状态；历史分叉或裁剪使旧保存位置不可达时保持未保存状态。浏览、过滤和面板布局不进入场景历史。

## 验收

`editor_document_acceptance` 不创建桌面窗口，默认运行中与工程资源相关的代表为：glTF 节点变换、左手系转换及法线/切线随变换；以及一次 OBJ 网格导入→重新导入，检查网格身份与槽默认材质绑定保持、旧几何不变、新贴图像素发布，且已保存的材质作者值不被覆盖。工程命令、浏览视图、导入失败回滚和工程搬移目前没有自动验收。

## 性能测量

JSON 当前位于打开、保存和显式目录刷新路径；帧绘制不解析 JSON。尚未建立不同解析库的同条件比较，不能据此宣称极限吞吐。优化取舍应同时看格式解析和真实 World 重建成本。

## 设计依据

- [Houdini 项目管理](https://www.sidefx.com/docs/houdini/basics/project.html)：目录范围与场景加载职责分开，使用相对项目位置的资源引用。
- [Godot 文件系统](https://docs.godotengine.org/en/stable/tutorials/scripting/filesystem.html)：项目根目录和资源路径形成可搬移的工作范围。本项目按用户明确方向使用 Config/Content/Source/Cache。
- [Windows 文件资源管理器](https://support.microsoft.com/en-us/accessibility/windows/use-a-screen-reader-to-explore-and-navigate-file-explorer-in-windows)：目录导航、内容选择、详情及视图切换分别承载清楚的任务。
- [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw) 与 [core/json](../../json.md)：当前文件替换机制与共享 JSON 合同。

## 工程源码与运行

工程的入口仍为目录。文件创建、Code 编辑、构建、属性、运行和诊断使用同一个 Project，所有菜单和快捷键走 Application 命令入口。Source 组件是正式 Runtime 类型；Parameters 和 Scene 使用同一字段元数据。

### 使用步骤

1. 打开工程，在 Project → New Component Source 创建 `Source/Move.source`。创建使用独占写入，已有文件不会被覆盖。Code 面板的 New 使用同一入口。
2. 编辑后 Ctrl+S。F6 或 Project → Build 编译所有工程 Source；外部保存由 500 ms 文件检查发现。新增类、方法、全局函数或 struct/enum 直接兼容发布。生命周期方法为 begin、tick（pre_physics）、post_physics_tick、late_tick、end，新增后在发布的同时绑定，Play 中也在帧间生效：已开始的对象从下一个阶段起执行新方法，不补发 Begin。修改已有类型的字段或默认值，或删除、改名、改变公开函数签名后，停止运行并选择 Rebuild Types。
3. 选择一个 Entity，打开 Window → Entity Structure，通过 Add Component 选择 `project.Move`。添加后自动检查该组件。Parameters 中公开数值使用单行参数编辑器，bool 使用 Checkbox，枚举使用命名选项菜单；结构页负责删除，属性修改与结构变化都进入场景文档。
4. 保存场景。F8 或 Run 启动独立游戏窗口；Shift+F8 或 Stop 停止。运行期间场景照常可编辑（创建、删除、改名、改父级、复制、字段与变换），修改即时转发给游戏窗口；它们是普通的文档修改，停止后仍在并且可以撤销。Project Output 显示构建、运行与退出信息。
   运行中单个对象的失败（Begin/Tick/End、任务、提取或命令被拒）只隔离该对象，Play 继续运行；每个失败写一行 `Diagnostic [严重度] kind in phase "Entity 名" (类型) 字段: 消息`，有源码位置时附 `at 文件:行:列`，持续中的同一失败不重复输出。修好代码并兼容发布后，故障对象在下个安全点恢复，不重发 Begin。
   编辑器自身的场景提取诊断（例如零缩放使网格不绘制）也写入 Project Output；同一行在消失后再次出现时，原行后面计数 `(×N)`，不新增行。
5. 运行时修改方法体并保存，兼容版本在下个安全点发布。语法或类型错误保留原代码；Code 的诊断可打开实际出错文件并定位行列。

示例包含两个普通 Source 文件，它们属于同一编译单元，不需要编辑器生成 C++ 包装：

```cpp
// Source/Math.source
float travel(float speed, float dt) { return speed * dt; }

// Source/Move.source
class project.Move : world.Component {
    float speed = 2.0f;
    bool moving = true;
    void tick(float dt) {
        if (self.moving) {
            self.translate_owner(travel(self.speed, dt), 0.0f, 0.0f);
        }
    }
}
```

`translate_owner` 将位移请求交给 World，在回调边界应用。含 `body.enabled=true` 的 Primitive、CharacterComponent 或 RagdollComponent 的 Entity 会拒绝此操作；停用组件不绕开已声明的物理所有权，物理移动使用 PhysicsScene 的命令。语言声明、原生派生和字段默认值的精确范围见[Source 类型声明](../../source-type-declarations.md)。

### 加载与执行链

```text
Project 配置 + Source 文件快照
  → Session::Prepare 注册原生 World / Physics 类型
  → ProjectCode → Source Program 准备并发布项目类型、行为
  → Scene v10 解析类型名、属性、资源和关系
  → 停止的编辑文档
  → Run：序列化当前场景到 Cache/Play，启动同一可执行文件的运行入口
  → 独立 Session → start → update → stop
```

Source 目录递归收集 `.source` 文件，排除 `.ui.source`，按相对路径排序，其后追加引擎附带的 `engine/` 源码（见[项目快照](../../source-function-modules.md#项目快照与多个公开入口)）。每份文件保留正文和版本；工程文件上限为 256 份，单份 1 MiB，工程与引擎文件总计 16 MiB。源码编译是同步、整工程候选发布；没有逐文件混合发布或磁盘机器码缓存。未改变内容不会重复编译。轮询依赖工程文件和引擎文件的修改时间和大小，显式 Build 会重新读取内容。

Session 在创建对象之前调用 Prepare，独占保有返回的代码拥有者。退出顺序保证 End 能访问行为代码，World 先于代码释放，Runtime 最后销毁。PhysicsScene 是 World 登记的模拟系统，Session::update 只调用 `World::update`，物理子步在其中推进。编辑器 UI Runtime 由 Workspace 独立拥有，场景替换不会让 UI 借用失效的运行时。

停止使用父进程创建、按句柄白名单继承的两个手动复位事件：父进程发送 stop，Runner 在 `runner.stop_observed` 处、`document.stop` 之前发送确认，随后正常收尾。确认只表示已响应停止，不表示资源已释放、日志已完成或 OS 进程已退出；父进程不解析日志或管道文本作确认。事件句柄随本轮 Play 的进程观察寿命释放，子进程释放自己的副本，进程树仍由 Job 管理。

Stop Play 先发送合作停止请求，UI 始终非阻塞轮询；从请求起 2 秒内既未退出也未确认才强制，收到确认后则到请求起 4 秒仍未退出才强制并报告错误。编辑器退出的同步 `PlayProcess::close` 用 500 ms 等待停止确认或退出；收到确认后允许正常收尾，但总退出观察预算仍是 close 入口起 4 秒，不从确认时重新计时。未确认分支强制后只使用总预算的剩余时间观察退出；预算耗尽时请求强制并记 `play.exit_unobserved`，不再增加等待。关闭仍最多读取八个固定输出预算，不等待管道数据；剩余尾部按可测量的放弃字节和未知损失记录。这些是父进程的响应与观察期限，不给子进程资源收尾本身增加硬上限，也不是整个 Application 的关闭截止时间；后续文档、UI、GPU 和日志清理仍按原顺序完成。

父进程首次观察确认记 `play.stop_acknowledged` info，`waited_ms` 是从首次停止请求到父进程观察确认的时长；强制请求记 `play.forced` warning，携带 `acknowledged` 区分未响应与收尾超时。`runner.started` 表示游戏文档已开始，不保证已完成首帧；此时立即关闭可以在第一帧被取消后以 0 帧正常停止，计数不为关闭补帧。

只有 OS 确认退出且取得真实 exit code 才记录 `play.process_ended`，包括强制退出；超时或观察失败记录带实际 waited_ms 的 `play.exit_unobserved` error，不伪造完成。未观测退出的句柄保留供后续非阻塞轮询，重复 close 和析构不重开等待预算；析构最终释放 OS 句柄也不等于子进程已正常结束。Runner 在帧间及[Renderer 合作停机边界](../renderer/contract.md#合作停机)读取同一停止请求，取消帧后沿正常 document.stop 和资源关闭路径退出，不承诺中断单个长驱动调用。

项目 Play 和 UI Preview 共用一个进程宿主。父进程传入工程位置、场景快照和停止事件，读取输出，并经变更链路转发文档修改（见下节）。Play 只使用场景里的显式主相机；没有可用主相机时黑屏，工程 UI 照常叠加并响应，不使用编辑器观察相机或自动取景。关闭编辑器或切换工程也回收该进程宿主。运行中的对象身份、GPU 资源和 JIT 地址不会跨进程传递。停止后保留编辑场景、选择和撤销历史；运行结果不会自动写回。

### 场景相机

- `world.CameraComponent` 派生 SpatialComponent，一个 Entity 至多一个（包括派生和停用组件），准入在挂接/恢复发布前检查。位置取 Entity 世界矩阵，朝向沿真实空间父链复合规范化局部四元数；缩放大小和符号不进入朝向，零缩放本身不使相机失效。灯光仍使用自己的 orientation 规则。
- 垂直 FOV 默认 45 度、near/far 默认 0.05/30000 米。反射硬边界为 FOV 1–179、near 0.001–100000、far 0.002–1000000，组件校验要求 far > near；渲染前仍验证有限性与投影。加载时同样执行联合校验，拒绝与默认值恢复规则见 [Scene 加载合同](../world/project-scenes.md#scene-v10)。当前格式中的可选字段未保存时使用默认值。
- `world.EnvironmentComponent.main_camera` 是指向 Entity 的弱引用，默认 null，保存为文档 `$ref`，不按顺序自动选择。写入必须走 Runtime 引用接口；同 Runtime 的跨 World 写入可以成功，消费端负责拒绝。每次更新结束（包括 late flush）重新检查引用存活、World 归属、Entity/祖先/组件启用、唯一 Camera、空间、投影和 focus；不依赖 World revision，不清掉失效引用，也不保活弱目标。
- **Create → Camera** 新建但不自动设为主相机；**Edit → Set as Main Camera** 将选中的同场景 Camera Entity 写入引用，可存停用目标。**Clear Main Camera** 清空引用；World Settings 的 Lighting 页显示当前选择并提供 Clear。作者 Set/Clear 只在本 Session 可编辑边界执行，各记一条历史；首次 Set 同一历史创建默认 Environment，Clear 在没有 Environment 时不创建实体。作者删除目标或包含它的子树时，同一删除历史同步 Clear；删除后保存再 Undo 仍同时恢复图和引用。
- **View → View from Camera** 一次性复制选中相机的位姿、roll、FOV 和裁剪面，不修改场景或撤销历史，不持续跟随。编辑器观察相机仍是视口状态。
- Play 的 lighting focus 是主相机世界位置，编辑器使用观察 target。切换主相机 Entity、从无效恢复、FOV/near/far 改变时提交 camera_cut；单纯转头不移动 focus。没有可用相机时输出诊断、重置 lighting clock，不提交 Lit 场景帧，UI 独立存在。游戏瞬移相机的显式 cut API 尚未提供。
- 游戏原生/生成代码可经 Runtime 写引用，变化只在 Play 副本内生效，不进作者历史、不自动回写。**真实 Source 控制器切镜头仍未实现**：Source 尚不支持所需的跨对象引用访问和写入，不能把 Runtime、原生或 IPC 的能力当作语言能力。

### Play 副本与变更批次

编辑器文档是唯一权威，Play 进程持有它的副本。副本加载后调用 `Session::enter_play()`：对象沿用编辑器分配的文档身份，副本自己不再分配，复制、导入等需要新身份的操作因此失败；游戏运行中生成的对象没有文档身份，只用运行时句柄寻址。

`Session::apply_live(batch, Project&)` 在宿主边界（帧循环中 update 之前）按顺序应用编辑器的一批修改，不写历史。批次为 `{"ops":[...]}`，身份是十进制字符串，值都是绝对值：

| op | 字段 | 语义 |
| --- | --- | --- |
| `add` | `parent`（身份或 null）、`index`、`graph` | 一个根 Entity 及其子树，graph 由 `serialize_live_subtree` 生成：保留身份，graph 外的引用写作 `{"$ref":身份}`。新对象按运行中插入的规则立即 Begin。 |
| `attach` | `entity`、`index`、`graph` | 给已有 Entity 增加组件。graph 中该 Entity 的记录为 `{"id","existing":true,"children":[],"components":[...]}`，由 `serialize_live_components` 生成。 |
| `remove` | `id` | 经 World 退出链销毁 Entity（连同子树）或 Component，End 照常执行；之后可以用同一身份再次 `add`（撤销删除）。 |
| `set` | `id`，可选 `name`、`enabled`、`space`、`properties`、`materials`、`sky_map` | `properties` 是稀疏记录，只写出现的叶，游戏写过的其他字段不受影响；记录按严格规则解码，越界即拒；标量叶作为一次组写入经过 `World::write` 的校验和准入，跨字段规则（例如整体平移一个探针体积）只对最终组合检查；String 与 Array 叶逐个整值写入。World 设置是 Environment 组件的属性，同样走 `set`；首次编辑时编辑器转发一条带完整组件的 `add`，撤销它转发 `remove`。`space` 为 null 时移除空间；记录中缺省的叶保持当前值，`origin` 取 `"0"`（世界）、null（继承）或身份。物理接管的 Entity 改位姿时由模拟系统瞬移。`materials` 是完整绑定表，不包含固定着色参数。`sky_map` 是 Environment 的天空贴图键，Play 在自己的 Project 中解析；目录里还没有这个键时先刷新一次 Project，仍解析不到时以对应资源种类的 warning 报告，键照常写入、天空没有贴图，不算分歧。`add` 中先前失败的天空贴图键会重试读取。 |
| `move` | `id`、`parent`、`index`、`space` | 以 keep-world 改父级和顺序，再写入最终局部 TRS。 |
| `materials` | `materials`（Scene 材质定义列表） | 已有身份按定义更新值；新身份创建后登记。绑定新材质前先发送它的定义。Play 读不到的来源、父材质或贴图与宽容加载一样以替代品应用，并报告警告。 |
| `resource` | `key`、`material`（v2 作者记录）、MS 必需的 `source: {path,text}` | 同键同角色原地发布 MS/MI，传入精确 Source 文本，不用接收时磁盘内容替换。引用它的实例随之重新解析，未加载的键登记供之后使用。 |
| `texture` | `key`、`width`、`height`、`file` | 读取本次发布的 RGBA8 spill，严格核对尺寸和字节数，更新同一 Texture 身份。 |
| `mesh` | `key`、`file` | 读取本次发布的 ATMS v1 二进制快照，核对键、几何、槽和默认关系；不回读项目模型，不重算已发布 normal/tangent。 |

ATMS 编解码的格式、文本、长度和预算拒绝返回 `invalid_storage`；解码只在完整校验后交付 MeshAsset，失败不会替换既有资源。v1 字节布局、4 KiB 文本/256 MiB 上限和 spill 剩余预算不变；`Mesh::restore_published` 验证已发布帧而不归一化，保留浮点位（包括负零）。

`index` 是对象在目标列表中的最终序号，null 表示末尾。`add`/`move` 的目标列表是父级的子级，`parent` 为 null 时是根列表；`attach` 的目标列表是该 Entity 的组件列表；`move` 计数时不含被移动的对象本身。游戏生成或销毁对象会让 Play 的列表合法地不同于文档，所以超出范围的序号夹到末尾，不报诊断。同一批中的多个 `add` 按（父级，序号）升序发送；Play 的列表与文档相同时，顺序与文档一致。

数组字段按整个数组传输：经过元素路径的修改和数组的增删、移动，都扩大为对该数组字段的一次 `set`，携带完整数组（随 Scene 的数组编码一起实现）。

返回值是未完全应用的项的诊断，phase 为 `command`，消息以 `Play change <序号> #<身份>` 开头：

- warning `reference_dropped`：目标已被游戏销毁，该项跳过；或某个引用的目标已被销毁，该引用写为 null，其余照常应用。
- warning `command_rejected`：Play 无法跟随这项修改，该项不生效，例如移动 Ragdoll 部件的祖先（见[物理](../physics/README.md)）。
- error `command_rejected`：应用失败，Play 已与文档分歧，需要重启 Play。`add`/`attach` 要么整项生效，要么不留下对象；`set` 在中途失败时已写入的部分不回滚。
- `add`/`attach` 携带文档本身的保留数据（占位类型、缺失资源等）时，按加载诊断的种类以 warning 报告，不算分歧。
- Play 不监听外部资源改写。`add`/`attach` 遇到缺失资源时刷新目录并重试一次，`resource` 的材质引用未登记的贴图时也刷新目录，以接上运行中新导入的资源。运行中在外部改写已登记的资源（包括网格元数据里的默认材质映射）不保证在本轮 Play 生效：按需刷新之后加载的网格可能已经取到新映射，已经打开的材质与贴图仍沿用会话中的版本。重启 Play 保证重新读取。

批次格式错误，或对编辑文档调用时，整次调用返回错误。

编辑器文档一侧由 `Session::set_live_forwarding(true)` 开始记录：每次提交、撤销和重做之后，按修改留下的当前值生成上表的 op，顺序与修改一致；新增或撤销删除的多个根按（父级，序号）升序生成 `add`，绑定新材质前先生成它的 `materials` 定义。预览和取消也生成 op，同一目标（Entity 的一个 TRS 叶、一组字段、一个材质或资源键）较新的预览替换队列中较早的一项；预览过的 TRS 叶在提交或取消时都会再发送一次，即使它回到了起始值。Body 设置作为整个 `properties.body` 记录发送。选择、编辑器观察相机和视口设置不转发；场景相机属性照常转发。`main_camera` 的 Set/Clear/Undo/Redo 只发送该引用叶（`{"$ref":"文档ID"}` 或 null），不相关标量修改不覆盖 Play 内的游戏选择。删除目标先发 Clear 再 remove，撤销先 add 再恢复引用；保留数据和引用历史同步回放。`take_live_ops()` 取走队列中的 op。某项修改无法描述时（例如没有持久来源的程序网格），记录停止，之后的调用一直失败，直到重新设置转发：这时 Play 已经与文档分歧。

多根导入及其 Redo 中，若本批新增或恢复的 Environment 的 `main_camera` 指向同批另一根下的 Entity，该叶在 Environment 的 add 中暂为 null，全部根 add 完成后再单独发送。它不依赖根的先后顺序；同根内部引用照常解码，其他引用字段不被消隐。批次外的既存 Environment 不被重发，游戏已删除且不在本批的目标不被重建。

Play 进程（`run_project`）加载场景后调用 `enter_play`，UI 预览除外。`RunOptions::channel` 可以挂一个 `PlayChannel`（不持有），帧循环每帧在宿主边界调用一次它的 `service(Session&, Project&, Report)`：首帧在 start 之前，之后每次都在 update 之前。批次的取得、应用、回执都在 service 内完成，service 不阻塞也不抛出；`Report` 与 World 诊断走同一条输出（`describe_diagnostic` 逐行写入 Project Output）。编辑器启动的 Play 经 `--live-channel` 挂上链路的 Play 端；独立运行的 `--run-project` 没有 channel。

Run 的私有快照固定 Scene 作者数据，不是全部工程文件的不可变副本；首载资源从已保存工程解析。图形 runner 在首个完整材质组 ready 后才 start；headless 没有 GPU 门槛。Live ACK 确认命令已经应用并且接收端已取得内容所有权，用于释放传输文件及更新命令同步状态，不代表 GPU ready、Present 或屏幕显示。渲染与拾取仍遵守各自的完整组及接受帧合同。

编辑器启动的 Play 由变更链路（[play_link.hpp](../../../modules/editor/application/include/application/play_link.hpp)）跟随文档：

- 传输：每次 Run 新建两条匿名管道，子进程一端的句柄只经 handle list 继承，以 `--live-channel <读> <写>` 传入。每条消息是 uint32 小端字节数加 UTF-8 JSON，单条上限 4 MiB。两端各有一个读线程和一个写线程，只搬运文本；编辑器 UI 线程和 Play 帧循环只轮询队列，不等待对方。
- 发送：Run 在序列化快照的同一步开始转发，快照与转发之间没有空隙。Play 还在加载时，批次留在管道中，在首帧 start 之前应用。编辑器每一步结束时（无论这一步从哪里返回）取走 `take_live_ops()`，作为一批发出：`{"type":"batch","seq":n,"ops":[...]}`，seq 从 1 递增。超过一条消息的批次写入本次运行的私有目录 `Cache/Play/<运行>/batch-<seq>.json`，文件内容正好是交给 `apply_live` 的 `{"ops":[...]}`，消息只带 `{"type":"batch","seq":n,"file":路径}`。文件始终由发送端拥有，在收到 ACK 或关闭本次链路时删除，接收端只读。超过 16 MiB 的批次在 op 边界拆成连续的几批；单个 op 超过 16 MiB 算作无法生成。Play 结束后编辑器删除该目录。
- 图片/网格：内容独立写入该目录的 `.rgba` / `.mesh`，命令只带路径。二者共用 256 MiB 在途资源预算，与 JSON 预算分开；未发出的失败候选清理自身文件，已发送内容保持到 ACK 或 close，不能以新参数 Undo 恢复旧资源版本。精确网格快照在后续目录刷新中仍保留，明确 Reimport 才替换。
- 回执：Play 按顺序对每一批回复 `{"type":"ack","seq":n,"diagnostics":[{"severity","kind","message"}]}`，诊断同时写入 Project Output。seq 不连续时回复 error 诊断 `sequence gap: expected batch k, received n`，之后的批次一律不应用；无法解析的消息回复 `{"type":"error","message":…}`。
- 分歧：以下任一情况表示 Play 不再跟随文档——回执带 error；回执乱序或无法解析；`take_live_ops()` 失败或 op 超限；未回执的批次超过 256 批或 64 MiB；Play 仍在运行而链路已关闭超过 2 秒（子进程退出时先关闭句柄，2 秒宽限避免把正常退出当成分歧）。分歧后停止转发，状态栏显示原因和 Restart Play 按钮（与 Project → Restart Project 相同：先 Stop，结束后重新 Run），Project Output 记一行。正常 Stop、Play 自行退出和 warning 回执都不算分歧；链路没有超时规则。
- 运行期间状态栏显示 `Play in sync`、`Play: n pending`（未回执的批次数）或 `Play diverged`。
- `{"type":"inspect","id":r,"object":身份}` 让 Play 回复 `{"type":"inspect","id":r,"graph":…}`（`serialize_live_subtree` 的文本）或 `error`。它只供验收使用，属于内部、不稳定的消息。

已知限制：`--report`（验收用）在停止后按文档身份序列化 Play 副本，而 Play 副本不给运行中生成的对象分配文档身份。停止时只要还有运行中生成的对象，`--report` 就显式失败，进程返回 1。P2 的运行时树改为按运行时句柄寻址时，与 `inspect` 一起解决。

命令行与编辑器使用同一个入口：

```powershell
atelier_editor --run-project E:\ffa_testproj
atelier_editor --run-project E:\ffa_testproj --preview-ui
```

`--halt-on-fault` 改用首错即停：第一个对象失败照常写出诊断行，随后停止运行并返回非零退出码，供验收与 CI 使用。编辑器启动的 Play 使用默认的隔离策略。

停止时输出帧性能报告，统计与编辑器 Performance 表的 CPU 段相同（见[帧性能](README.md#帧性能)）：`Performance: N frames (window n), frame mean … ms, p95 … ms, max … ms, … FPS` 是最后一个窗口的整步统计；`Performance phases (mean ms): …` 列出进入过的顶层阶段（`app.*`、`world.update`、`render`）和 unattributed 的均值；存在计时结构错误时另起一行给出数量和第一个出错的阶段。没有接受过画面时只输出 `Performance: N frames, none accepted`。数值只用于诊断，不作为验收条件。

### 项目 UI

Project → New Project UI 创建 `.ui.source` 并写入 `Config/Project.json` 的 `ui_source`。Edit Project UI 打开该文件，Preview Project UI 使用独立预览窗口；游戏窗口也加载同一个入口：

```cpp
ui ProjectUI(in Text title, in double elapsed, in double entities) {
    Column hud {
        padding_left: 8; padding_top: 8; gap: 4;
        Text title { text: input.title; }
        Text hint { text: "Edit this UI in your project."; }
    }
}
```

三个输入分别为工程名、运行秒数和 Entity 数量。项目 UI 复用 Source/JIT、LiveDocument、原生 UI、SS、字体、输入和 renderer。预览不启动游戏行为；游戏中 UI 与场景分别拥有 Runtime，只交换输入值。

编辑器对项目 UI 只做编译检查，不求值表达式和事件。用户 UI 代码在子进程执行，所以表达式或事件陷入死循环也能停止。保存兼容 UI 变更保留局部状态。ProjectUI::prepare 统一完成源码、控件和真实布局准备，成功后才发布候选；错误恢复上次成功定义、状态与控件。窗口最小化或零 Surface 时暂停发布；冷启动无有效版本时报告失败。入口配置变更在下一次启动运行或预览时采用。

File → Edit Editor UI Source 是引擎开发命令，用于修改编辑器自身的受信任 UI；它与项目 UI 的执行边界不同。SS 仍按启动加载策略生效。

### 保存、失败与重建

- 未保存源码保留在 CodeDocument 中；构建、运行和预览命令先保存当前工程的已修改 Source 文档，磁盘冲突按原保存流程处理。
- 方法体更新保留对象、字段和场景历史；稳定 Entry 在下一次调用使用当前代码，旧 Lease 保留其代码。构建失败不替换最后成功版本。
- Rebuild Types 在当前无占位对象时先尝试兼容热更（仅改方法体时直接原地生效，不清空历史）；确需重建类型时，从当前内存场景值与全部已打开程序化图草稿（含未落盘 `.procgraph` 与 `scratch` 临时图，不静默覆写磁盘 `.procgraph`）准备新 Session 并同步生成器，成功才替换文档。它保留源码缓冲和源码撤销、未保存场景与未保存图的 `dirty` 标记、视口相机姿态、当前打开的 SOP 图上下文、`NetworkCanvas` 平移缩放以及场景实体/节点选择，仅清空依赖旧对象身份的场景、图与材质撤销历史；存在任一方向历史时交互提示确认，无任何 Undo/Redo 历史且无活动交互时保存 Source 或按 F6 遇到 `rebuild_required` 会自动完成保状态重建。
- 删除已保存字段、改变字段类型或移除仍在场景中使用的类型时，Rebuild Types 仍然成功：旧值作为未知字段、不符值或占位对象保留，保存原样写回；恢复字段或类型后再次 Rebuild Types 即还原。只有编辑该字段或 Discard Saved Data 才丢弃旧值。
- 交互打开工程或场景时源码构建失败：仍进入工程和 Code，场景以占位对象加载（工程类型全部缺失，原生部分照常显示），可以编辑和保存，保存按原类型写回；不沿用上次成功构建的类型。构建失败期间 Run 不可用。场景对象创建后 Runtime 不再接受新类型，所以修复源码后由一次新加载恢复真实类型：没有场景 Undo/Redo 历史时自动重新加载，否则提示 Rebuild Types（清空场景历史）。严格打开在构建失败时直接拒绝。
- Cache 只放临时运行快照。关闭正常运行后删除本轮快照；源码、配置、资源和场景均保存在工程目录，删除 Cache 或搬移目录后可重建关联。

### 验收与范围

`project_integration_acceptance` 使用真实磁盘、Source 编译器、Runtime 和独立运行进程，覆盖一次跨文件 Source 组件的 tick 结果，以及停止时死循环的 Source 子进程在 5 秒内被强制结束。

`project_scene_acceptance` 覆盖一次真实注册类型字段值的 Scene 保存/重开往返，以及失败 Tick 中嵌套 stop 时 End 返回后才释放物理查询状态的停止顺序。运行命令与日志位置见[源码工作流验收](source-editor.md#端到端验收)。

当前提供独立运行窗口和 UI 预览。[Runtime/JIT 调试核心](../../source-debugger.md)已实现断点、单步和只读变量检查；编辑器中的交互与 Control 指令仍处于[接入方案](control.md#源码调试接入待实现)阶段。语义补全、查找替换、导出打包、运行画面嵌入编辑器及运行值回写尚未实现。

## Sponza 完整模型

[Sponza](../../../examples/Content/Sponza/README.md) 与 Scene Lit Room、Physic、Dungeons 同属根目录的普通 [Examples 工程](../../../examples/README.md)，不随引擎部署。无参数启动为空文档；`--project ./examples` 打开工程配置指定的 Sponza，`--scene Content/...` 可明确选择其他场景，不必同时传相机参数。资源浏览器按 `Project / Content / 场景名` 展示目录，场景只保存到 Content。打开场景不自动改变视口模式或拟合 DDGI；每个场景保存自己的主相机，Play 使用该相机。需要固定编辑视图时，可选择 Main Camera 后用 View from Camera，或显式传入 `--material-camera` / `--scene-lit-camera`。

`editor_log_smoke_acceptance` 打开 Examples 副本的启动场景并做一次添加/保存/撤销冒烟，见[验收入口](README.md#验收入口)。材质与视口显示模式的渲染代表见 [Renderer 验收](../renderer/acceptance.md)。

## Dungeons 地牢资源

[Dungeons](../../../examples/Content/Dungeons/README.md) 包含完整 Demo 和两个资源总览，使用原始世界变换生成独立 Entity。通过普通 `--project ./examples --scene Content/Dungeons/Scenes/Dungeon.scene` 打开；编辑视口可选择 Smooth 查看贴图。转换后的 2314 个模型资产可通过现有 Asset Browser 添加到场景。
