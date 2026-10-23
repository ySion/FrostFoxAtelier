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
- Ctrl+S 保存当前工作区域：Code 区保存当前源码，其余区域保存场景；File → Save Scene 始终保存场景。Ctrl+Shift+S 另存场景到当前工程 Content 或 Examples 内，File → Open Scene 打开同一工程中的场景。标题显示工程、场景和未保存标记。
- 关闭窗口、打开其他工程或场景前处理未保存修改。保存决定完成后才读取目标场景，取消新建也不会创建目录。加载在独立、停止的新 Session/World 中完成；失败保留当前文档，成功才撤下旧 UI 绑定和文档。
- Config 的 `startup_scene` 是打开工程时加载的场景，路径相对工程根。另存场景不隐式修改启动场景配置。
- Source 文件由工程统一采集、编译并绑定到真实场景类型。Project 菜单提供创建组件源码、构建、重建类型、运行、停止和项目 UI 预览。

## 异常退出时的场景保全

Application 捕获帧循环异常（包括 Renderer 返回的终态错误）后，在 Session 释放前尝试一次保全；帧循环外的异常栈展开由析构入口兜底。正常关闭沿用原来的保存/放弃流程。

未确认的参数或变换手势先取消，再使用同一个 `Session::serialize` 保存最后已提交的作者 Scene。输出为 `%LOCALAPPDATA%/FrostFoxAtelier/Recovery/<唯一标识>/Scene.scene`，日志以 `Scene recovery saved:` 报告完整路径。写入复用原子文件创建，不覆盖原场景、不标记文档已保存；重复失败不会重写快照或替换首次原因。

Scene 先写入并刷新到磁盘，随后 `recovery.json` 记录原工程、原场景、失败原因、时间、未确认手势与脏材质资源名称。侧记写出失败时仍保留已经写成的 Scene，并输出警告。序列化或磁盘失败明确输出 `Scene recovery failed:`，不宣称恢复成功。

这条路径不复制或解析资产，缺失 mesh 等外部资源不会阻止有效作者 Scene 的原始引用写出。它不是完整工程备份：独立材质资产、Source 文本草稿和没有持久化来源的程序化网格不在本次保全范围内；原 Scene 序列化本身拒绝的状态仍会失败。强制结束进程、访问违规或断电不能保证执行异常保全。

恢复时先保留原工程及快照，将 `Scene.scene` **复制为原工程 Content/Scenes 下的新文件名**，再通过同一工程的 Open Scene 验证类型和资源；确认后再决定保存位置。不要直接覆盖原文件。

独立 CPU 端到端入口：`cmake --build --preset dev --target editor_recovery_acceptance`，发布配置对应 `release`；也可直接运行 `atelier_editor_document_acceptance --recovery`。它使用真实 Session、作者场景序列化、原子文件写入和重新加载，覆盖未确认手势取消、缺失资源、幂等和 IO 失败，不创建设备或启动窗口。真实设备失效后的 Application 调用链需在 GPU 故障风险消除后复验。

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

## 资源浏览器

原 Asset Shelf 沿原停靠视图演进为 Asset Browser。左侧常驻目录树以 Project、Engine 为并列根：Project 展示实际 Content、Source、Config 及可选的 Examples 目录，Engine 展示只读内置网格；未打开工程时 Project 根禁用。路径与搜索位于同一紧凑工具行，路径菜单可访问祖先目录，返回和上级操作使用同一导航状态。类型筛选、List/Details/Tiles 和资源操作各由图标菜单承载。

目录树与结果区之间使用 DSL Splitter，可拖动调整目录宽度；两边分别保留至少 96/64 DIP。Esc、失焦、捕获丢失或关闭正在编辑的区域会取消本次调整；临时缩窄整个面板只限制显示宽度，不抹掉用户此前选择的宽度。目录宽度沿现有工作区状态保留，不进入场景撤销，不触发 viewport 渲染目标缩放。

单击资源改变浏览器与参数检查目标，不修改 Scene Tree 选择或创建 Entity。Material 资源打开共享参数编辑，其他资源显示元数据。更多菜单及资源右键菜单中的 Open 进入目录或打开场景，Details 的双击/Enter 走同一 Open 入口。Add to Scene 执行既有 StaticMeshEntity 创建命令；Import OBJ、Refresh 也沿原命令入口。右键先选中当前资源，再构建菜单；原资源消失或选择改变后关闭旧菜单。切换视图保留所选资源、过滤、排序和列宽。Tiles 使用类型图标，不声称已经实现模型缩略图。

查询或类型过滤排除当前资源时立即清空选择，并禁用对应 Open/Add；仍匹配的选择保留。清除过滤不恢复先前的隐藏选择。目录导航使用共享 Tree 的展开、选择与键盘行为；从路径或内容进入目录时显露对应树行。视图和类型菜单使用互斥单选标记。目录刷新淘汰失效导航记录，当前目录消失时回到有效来源根。

Details 使用共享 Table 的 Name / Type / Size 列，列头显示排序方向；Size 按原始字节数排序、右对齐，目录始终排在文件前。列边界拖动有最小宽度，松手保存本次列宽，Esc/失焦/捕获丢失取消。窄面板保留各列并提供横向滚动。整表是一个 Tab 停靠点，方向键、Home/End/PageUp/PageDown 移动当前单元格并显露所选行。排序保持资源身份；选择变化复用原数据快照。详细控件合同见[共享表格](../ui/README.md#表格)。

工具行、目录树、内容槽和状态行由 `AssetBrowserShell` 声明，List/Details/Tiles 均由 `AssetResults` 的同一个 Table 节点声明，均位于 [editor.ui.source](../../../assets/editor.ui.source)。保存可更新这些区域的布局，同时保留兼容的控件身份、选择、排序列宽和搜索草稿。浏览器只提供目录/资源快照和既有命令；目录模板按实际变化缓存。全部结果由共享 Table 按当前视图虚拟绘制，原浏览器虚拟列表、合成资源按钮与逐项菜单选择路径已移除。

Import OBJ 导入 Wavefront OBJ 的位置、法线、UV、MTL 漫反射材质及依赖贴图。简单多边形（每面最多 64 个角点）按投影耳切三角化，保留原边界 mask；支持正/负索引，缺少法线时生成面法线，带贴图的面必须提供有效 UV。OBJ 的 V 轴转换到图片左上原点，超出 [0,1] 的 UV 保留并重复采样。单独 OBJ 保持 `Content/Meshes/<文件名>` 的位置；带依赖的模型使用 `Content/Meshes/<模型名>/` 独立目录，保持相对依赖关系。新增带材质的 Entity 使用白色实例颜色，避免默认蓝色染色。

MTL 支持 `Kd`、`d`/`Tr`、`map_Kd` 与 `map_d`。WIC 解码 PNG/JPEG/BMP 等系统支持格式；颜色贴图按 sRGB 采样，遮罩按线性值读取。独立 Material 资源支持金属度/粗糙度、法线与 Y 轴约定、HDR 发光和 Mask，并由 Scene Lit 的阴影、反射与 DDGI 消费。OBJ/MTL 的具体映射、通道及着色范围见[材质系统](../renderer/materials.md)；本轮不包含高度贴图或半透明混合。带 `-s`/`-o` 等选项的 map 明确报错，动画仍未导入。

依赖只能位于 OBJ 所在目录及子目录；绝对路径、上级路径、逃逸链接、缺失文件与非法数据在发布前拒绝。单文件最多 128 MiB、整包最多 512 MiB、解码像素最多 768 MiB，图片单边最多 8192；最多 512 个依赖文件、1024 个命名材质、100 万个位置/法线/UV 和 100 万个三角形。导入验证和写盘使用相同字节快照，先检查现有目录，再独占创建依赖、网格与身份文件；已有同名文件不覆盖。发布后目录检查失败时仅删除本轮创建的文件，允许修复后重试；空目录可能保留，清理失败明确报告。

Refresh/F5 重新扫描目录，不在 UI 每帧访问磁盘。网格 UUID 与依赖文件的大小/修改时间共同决定缓存是否有效；未变化刷新保留 revision 和资源，MTL/图片改写或删除淘汰相应网格解析缓存。已有 Entity 保有旧 Mesh/图片版本，重新打开场景或再次添加资源取得新版本。Project 内多个 OBJ 引用规范路径与内容一致的贴图时共享不可变解码图像；缓存保留源字节和图像弱引用，刷新清理已失去拥有者的条目。修改图像后新旧 Mesh 各自保有对应版本。外部工具应正常更新文件修改时间；刻意同时保持大小和时间的内容改写无法识别。


每个 OBJ 旁的 `<文件名>.asset.json` 保存版本和持久 UUID。移动、重命名资源时须一同移动该元数据，并保持 MTL/贴图的相对引用；复制资源应使用 Import，以获得独立身份。Refresh 为新增 OBJ 建立元数据，重复 ID 或非法元数据明确报错。元数据与资源共同位于 Content 或 Examples，需要与源文件一同保存到版本控制；Cache 不承担资源身份。目录索引首片最多 10000 项；`.asset.json`、Unity `.meta` 和临时文件不单列于目录索引，原文件保留。查询结果按目录版本缓存；List/Details/Tiles 共用 Table 的可见单元格绘制，不为整个目录建立布局节点。

## 材质与贴图资源

File 或 Asset Browser 菜单中的 New Material 创建 `Content/**/*.material`，Import Texture 导入 PNG/JPEG/BMP/TIFF 到 `Content/Textures`。二者与 Mesh 共用 Project 目录索引及 `.asset.json` GUID，类型筛选增加 Materials、Textures。材质文件保存参数、六路贴图 GUID、UV 变换、寻址与通道；图片解码复用 WIC。路径、缺失引用、错误资源类型、重复身份和非法参数均在发布前验证。

选择 Material 后，Parameters 使用 Runtime `material.Material` 的实际反射字段：颜色/向量共用 NumberRow，开关和枚举使用既有控件，贴图由资源菜单选择。标量/颜色和采样设置沿 Session 的 begin/preview/commit/cancel 编辑链；菜单赋纹理也记录完整事务。每个材质资产拥有独立撤销历史与保存点，Ctrl+S 保存当前材质；Save All 保存材质、源码和场景。运行与重建类型先提交并保存材质草稿，避免新 Runtime 或独立进程读取旧文件。关闭、切换文档处理所有未保存资产；Control 替换文档直接拒绝未保存资产。

Entity/Component 的 Parameters 按网格槽显示材质选择菜单。菜单提供赋予共享 Material、Edit Shared Material 和 Use Mesh Default。已有资源绑定时不再显示会被覆盖的旧组件颜色/粗糙度字段。绑定的参数行旁有覆盖复选框：勾选后在同一 Runtime 惰性创建具有父材质强引用的实例；只记录被勾选参数的覆盖位，未覆盖参数继续继承。取消某项覆盖立即恢复父值。当前支持一层继承，覆盖颜色、透明度、粗糙度、金属度、发光、法线强度/Y 约定及覆盖/双面标志；纹理及采样设置从共享材质继承，未提供实例纹理覆盖。

例如两根柱子引用同一材质，仅 A 覆盖 roughness 后，修改父材质 base_color 会同时更新 A/B；A 保留自己的 roughness。资产修改不污染场景历史；槽赋予、实例参数及覆盖开关进入场景历史。复制 Entity 共享资产但复制其可变材质实例。更换网格保留名称唯一匹配的绑定，丢失/歧义槽被移除；清空网格清空材质引用。没有名称的槽使用 `slot/<index>`，命名槽按名称持久对应。

编辑器的内联实例参数历史目前要求单组件归属；同一匿名实例绑定多个组件时，参数编辑明确拒绝。其共享身份仍可保存和复制，原生消费者通过 `material::update_material` 更新同一个资源对象。

资源引用由 Runtime 强引用与 GC 管理；Project 保留不可变图像解析缓存和持久索引。渲染只借用拥有型 Surface 快照，不访问 Runtime 对象或磁盘。材质变化按内容发布快照，未变化帧复用；对象关闭后已发布捕获仍拥有图片内容。实例原生字段编辑须经过 `material::write_property / set_override` 明确覆盖意图，不能仅根据字段值与父值不同推断覆盖。

外部图片改写目前不会自动替换已载入 Runtime 中的 Texture 对象。Project Refresh 更新目录及解析缓存；已有会话保留既有图片版本，重新打开工程/场景重新解析。替换为其他 Texture GUID 会立即使用对应资源。本轮没有接入 UI Image 控件的 Runtime Texture 资源。

Scene 统一写出 v7：顶层 `materials` 按文档 ID 保存材质定义，StaticMeshComponent 的槽绑定引用同一 ID 并保留赋予意图；实例保存父 GUID、显式覆盖名称和值。共享匿名材质往返后仍是同一个对象。旧逐槽 payload 仅在读取边界兼容，完整格式见[项目场景](../world/project-scenes.md#scene-v7)。Runtime ObjectId、CPU/GPU 地址不进入文件。Control 复现导出在独立副本中写入已载入材质的实际草稿，验证贴图与当前载入内容一致，不保存原工程。

新增 `editor_material_acceptance` 是真实 Project → Runtime → World → Session → SceneSnapshot/DSL → 文件重开的端到端消费者；同时应运行原 document/project scene 消费者和 renderer GPU 材质验收。新增用例不等于已验收通过，实际结果以本次集成日志为准。

## 持久格式与责任

- Project 和资源身份文件为 UTF-8 JSON、`version: 1`；Scene 写出 `version: 5`，继续读取 v1/v2/v3/v4。JSON 解析由锁定的 nlohmann/json 提供。共用内部格式校验：16 MiB 上限、最多 32 层容器、禁止重复字段、身份及版本必须是非负整数、向量必须精确匹配长度且各项有限。Project 与资源身份文件拒绝未知字段；Scene 的未知字段与不符值按下述加载策略处理。内部 JSON 结构不穿过 Project/Session 公共接口。
- Project 保存名称、相对 `startup_scene` 和可选的 `ui_source`（Source 内的 `.ui.source` 入口）。目录解析检查绝对路径、上级路径和解析后逃出工程的链接，不以进程当前目录寻找资源。
- Scene v7 保存有序 roots 与 Entity records、真实类型、名称、enabled、有序 children/components、共用的字符串 uint64 文档 ID、可选内联 space、材质和资源键。组件不保存空间 root/parent/TRS。存档不写 Runtime 的 TypeId/ObjectId/FieldId、内存地址或 GPU 资源。
- World 光照、效果与天光是 Environment Entity 上 `world.EnvironmentComponent` 的普通属性，随该组件保存；字段与 UI / Control 共用的 author-option 目录一一对应，包括 `direct_candidates`。没有 Environment 时使用默认值，文件中不生成它。`World::environment()` 是权威值；`Session::set_environment` 是 World Settings 与 Control 的唯一编辑入口，比对候选中的 World 选项，把变化的项写成一次组写入，记一条普通属性编辑历史；没有 Environment 时同一历史项先创建它，撤销即回到没有 Environment 的状态。无变化为空操作；运行中或关闭的 World 拒绝；Environment 存在但停用（组件或任一祖先停用）时也拒绝，原因提示先在 Parameters 里启用它，因为此时 World Settings 显示的是默认值。v6 的根 `world` 段与 Sky Light 在读取时并入 Environment，见[项目场景](../world/project-scenes.md#v6--v7-迁移)；v1–v3 使用默认值。v4 的 `shadow_light_budget` / `shadow_samples` 仅在兼容读取边界严格校验后丢弃，v5/v6 不接受这两个旧字段；`direct_reference` 始终是视口诊断，不进入 Scene。
- 编辑器渲染快照组合 World authoring 与视口偏好；Project Run 从载入的 World 读取同一配置，并复用 `SceneSnapshot` 投影真实材质与灯光；天光由宿主按 Environment 求出 `Frame.environment`，没有 Environment 或天光关闭时为零。探针滚动位置、历史、统计、可视化与相机不写入 Scene。
- TRS 按 `position` / `scale` 的 xyz 数组和 `rotation` 的 xyzw 数组保存，颜色同样是数值数组；不保存数学类型名或原生内存布局。统一到 `math.vector3` / `math.quaternion` 后，float 精度、分量顺序、局部变换与行向量组合语义保持一致。旧 Scene v1–v5 经严格导入进入 Entity 模型，保存统一写出 v7，旧 `actors` 目录与 world.Actor / world.StaticMeshActor / world.SceneComponent 名称只在严格导入边界解释，不作为现行 Runtime 或 Control 别名。
- Scene 按限定类型名恢复已注册的 Entity/Component 派生类型；公开标量和内联记录进入 `properties`，新增未保存字段保留 Runtime 默认值。Physics Body 设置沿 `properties.body` 保存；普通引用为 null 或 `{"$ref":"ID"}`，序列为引用数组，持有/观察由 Runtime schema 决定。未知版本、失效关系、身份冲突和组合/空间环拒绝整个场景；未知类型、未知已保存属性、类型不符的值和缺失资源由宽容加载保留（占位对象、默认值加原值、占位资源、诊断），由严格加载拒绝；无法编码的字段保存失败；空网格保留为空。单场景最多 8192 Entity、每个 Entity 最多 1024 组件、文件最多 16 MiB。详见[项目场景合同](../world/project-scenes.md)。
- 加载策略分两种。宽容加载用于交互打开工程、场景和示例，以及 Rebuild Types：无法应用的数据按对象保留，诊断写入 Project Output（一行汇总加逐项列表）；Scene Tree 以琥珀色标出含保留数据的 Entity，Entity Structure 与 Parameters 把占位对象显示为 `类型 (missing type)`，Parameters 的 Saved Data 组只读列出保留值，Edit → Discard Saved Data 丢弃所选对象的保留值（可撤销）。缺失的 Material 或 Texture 在材质槽和贴图输入中显示为 `(missing) 键或名称`，目录中仍列出但无法读取的显示为 `(unreadable) …`；Mesh 不可用的组件多一行只读 `Mesh`，其材质绑定表列在 Saved Data。不可用按资源种类记录：场景把材质的键写成 Mesh 引用时，只有该 Mesh 不可用，材质照常打开、编辑和保存；贴图文件无法解码时，引用它的材质也照常保存。保存原样写回保留数据和不可用资源的引用，状态栏给出保留项数量；目标已删除的保留引用写为 null 并在 Project Output 报告。严格加载用于可复现流程：`ProjectOpenPolicy::require_scene`（`--profile-render-performance` 打开与 Control 复现导入）遇到任何诊断都拒绝。Play 子进程宽容加载，开始时输出一行汇总；占位对象没有行为。Control 的 `editor.inspect` 对占位对象另报告 `placeholder: true` 与 `saved_type`，单对象详情列出 `saved_data`，网格组件带 `mesh.source` / `mesh.missing`，材质槽带 `missing`。v6 中未知或不符的 World 设置随迁移成为 Environment 组件的保留数据，显示在它的 Saved Data。当前限制：无法并入 Environment 的部分（`world` 段整体不是对象，或未知键与 Environment 已用的字段名相同）作为文档级保留数据，只在 Project Output 报告，面板中没有对应显示。
- 物理角色、布娃娃/部件、车辆/车轮、力和运动学组件复用上述属性格式。DistanceConstraint 的 `properties.target` 保存统一文档观察引用，RagdollComponent.parts / VehicleComponent.wheels 保存显式 Entity 持有引用数组；全部对象创建后恢复引用并校验，场景外目标拒绝，不保存后端句柄。旧 constraint_target 序号仅在 v1–v5 导入时解析。当前使用方式见 [Physic 示例](../../../assets/examples/Examples/Physic/README.md)。
- SceneQueryComponent 复用 Scene v7 保存 TraceSettings、局部端点和自动/忽略自身开关；枚举使用名称，运行中的查询请求与命中结果不写入文档。重新打开必须重新采样，详见[场景查询](../physics/scene-queries.md)。
- StaticMeshComponent 自身保存反射可见的 `mesh_source`；World 的 `set_mesh_binding` 一次替换来源和解析内容，`read_static_mesh` 覆盖 Detached 与空网格。裸 `set_geometry` 清空旧来源。编辑器不维护 Component ID 到资源键的权威副表。
- 重建先创建全部真实 Entity/Component，恢复值、资源与普通引用，再恢复有序组合和 Entity 空间参考，完整 forest 经统一批次注册；无效关系拒绝候选。加载 StaticMeshEntity 不重复调用带默认组成的快捷工厂。
- 保存先完成有效文本草稿，经同目录临时文件、FlushFileBuffers 和替换发布。写入失败保留旧文件；这不是跨文件事务或断电恢复保证。工程标记最后创建，未完成创建不会伪装成可打开工程。存档提交与目录刷新结果分开：成功保存后即更新保存点，资源刷新失败显示警告，不将成功写入误报为保存失败。
- 场景撤销回到保存位置后恢复干净状态；历史分叉或裁剪使旧保存位置不可达时保持未保存状态。浏览、过滤和面板布局不进入场景历史。

## 验收

`editor_document_acceptance` 还直接运行真实 Project → Workspace → UI 布局/绘制/命中链，在两种逻辑窗口尺寸与 1/1.5/2 DPI 下切换三种视图和滚动选择，不创建桌面窗口。文件链覆盖真实目录创建/打开、中文名、OBJ 导入、元数据身份、组件和空网格往返、保存点撤销、坏版本/坏 JSON、写入失败、资源改名、工程搬移、Cache 删除及无来源网格拒绝保存。

同一验收还覆盖非法数值身份、向量长度、重复键、过深/超限输入、独占发布、坏目录导致的导入重试，以及 10000 项目录导致发布后失败时的文件回滚。刷新无变化、文件改名和真实内容改写分别验证缓存复用、旧版本生命周期与新版本解析。

固定 Scene v1 文件独立于当前序列化器，经过真实读盘、Session/World 重建、父子矩阵计算、资源解析、保存与重开，核验统一数学类型后的格式兼容性。

普通 `editor_acceptance` 通过同一应用检查工程命令、Ctrl+S 提交字段、项目重开、三种浏览视图、搜索和类型过滤、资源选择与场景选择隔离、导入定位、坏场景保留当前文档及工程搬移。每轮使用独立证据目录。

## 性能测量

`out/release/atelier_editor_document_acceptance.exe --profile-json <新证据目录>` 使用现有消费者创建独立工程，测量 1/64/256 Entity，以及 256 Entity、每个 16 组件的真实存档。每项预热 2 次、采样 21 次，记录文件字节数、中位数、P95 与最大值。分别测量暖文件缓存读取、原始 DOM 解析、含输入边界检查的解析、DOM 输出、场景序列化、从文本重建和读盘重建。DOM 与文档销毁在计时外，不包含窗口、GPU 或磁盘冷读；没有测量解析器独占内存峰值。测量目录不得指向用户实用工程。

JSON 当前位于打开、保存和显式目录刷新路径；帧绘制不解析 JSON。尚未建立不同解析库的同条件比较，不能据此宣称极限吞吐。优化取舍应同时看格式解析和真实 World 重建成本。

## 设计依据

- [Houdini 项目管理](https://www.sidefx.com/docs/houdini/basics/project.html)：目录范围与场景加载职责分开，使用相对项目位置的资源引用。
- [Godot 文件系统](https://docs.godotengine.org/en/stable/tutorials/scripting/filesystem.html)：项目根目录和资源路径形成可搬移的工作范围。本项目按用户明确方向使用 Config/Content/Source/Cache。
- [Windows 文件资源管理器](https://support.microsoft.com/en-us/accessibility/windows/use-a-screen-reader-to-explore-and-navigate-file-explorer-in-windows)：目录导航、内容选择、详情及视图切换分别承载清楚的任务。
- [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw) 与 [JSON 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0)：当前文件替换机制与固定依赖来源。

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
  → Scene v7 解析类型名、属性、资源和关系
  → 停止的编辑文档
  → Run：序列化当前场景到 Cache/Play，启动同一可执行文件的运行入口
  → 独立 Session → start → update → stop
```

Source 目录递归收集 `.source` 文件，排除 `.ui.source`，按相对路径排序。每份文件保留正文和版本；上限为 256 份、单份 1 MiB、总计 16 MiB。源码编译是同步、整工程候选发布；没有逐文件混合发布或磁盘机器码缓存。未改变内容不会重复编译。轮询依赖修改时间和大小，显式 Build 会重新读取内容。

Session 在创建对象之前调用 Prepare，独占保有返回的代码拥有者。退出顺序保证 End 能访问行为代码，World 先于代码释放，Runtime 最后销毁。PhysicsScene 是 World 登记的模拟系统，Session::update 只调用 `World::update`，物理子步在其中推进。编辑器 UI Runtime 由 Workspace 独立拥有，场景替换不会让 UI 借用失效的运行时。

项目 Play 和 UI Preview 共用一个进程宿主。父进程传入工程位置、场景快照、停止事件和当前视口相机，读取输出，并经变更链路转发文档修改（见下节）。编辑器启动的 Play 从当前视口相机开始；命令行独立运行没有相机参数，对准各网格位置的中心，避免大面积碰撞地面把运动物体缩得很小。Stop 先请求正常退出；2 秒未响应后结束该运行进程组。关闭编辑器或切换工程也回收它。运行中的对象身份、GPU 资源和 JIT 地址不会跨进程传递。停止后保留编辑场景、选择和撤销历史；运行结果不会自动写回。

### Play 副本与变更批次

编辑器文档是唯一权威，Play 进程持有它的副本。副本加载后调用 `Session::enter_play()`：对象沿用编辑器分配的文档身份，副本自己不再分配，复制、导入等需要新身份的操作因此失败；游戏运行中生成的对象没有文档身份，只用运行时句柄寻址。

`Session::apply_live(batch, Project&)` 在宿主边界（帧循环中 update 之前）按顺序应用编辑器的一批修改，不写历史。批次为 `{"ops":[...]}`，身份是十进制字符串，值都是绝对值：

| op | 字段 | 语义 |
| --- | --- | --- |
| `add` | `parent`（身份或 null）、`index`、`graph` | 一个根 Entity 及其子树，graph 由 `serialize_live_subtree` 生成：保留身份，graph 外的引用写作 `{"$ref":身份}`。新对象按运行中插入的规则立即 Begin。 |
| `attach` | `entity`、`index`、`graph` | 给已有 Entity 增加组件。graph 中该 Entity 的记录为 `{"id","existing":true,"children":[],"components":[...]}`，由 `serialize_live_components` 生成。 |
| `remove` | `id` | 经 World 退出链销毁 Entity（连同子树）或 Component，End 照常执行；之后可以用同一身份再次 `add`（撤销删除）。 |
| `set` | `id`，可选 `name`、`enabled`、`space`、`properties`、`base_color`/`roughness`/`metallic`/`emissive`、`materials` | `properties` 是稀疏记录，只写出现的叶，游戏写过的其他字段不受影响；记录按严格规则解码，越界即拒；标量叶作为一次组写入经过 `World::write` 的校验和准入，跨字段规则（例如整体平移一个探针体积）只对最终组合检查；String 与 Array 叶逐个整值写入。World 设置是 Environment 组件的属性，同样走 `set`；首次编辑时编辑器转发一条带完整组件的 `add`，撤销它转发 `remove`。`space` 为 null 时移除空间；记录中缺省的叶保持当前值，`origin` 取 `"0"`（世界）、null（继承）或身份。物理接管的 Entity 改位姿时由模拟系统瞬移。`materials` 是完整绑定表。 |
| `move` | `id`、`parent`、`index`、`space` | 以 keep-world 改父级和顺序，再写入最终局部 TRS。 |
| `materials` | `materials`（Scene 材质定义列表） | 已有身份按定义更新值；新身份创建后登记。绑定新材质前先发送它的定义。 |
| `resource` | `key`、`material`（`.material` 资产内容） | Play 已加载的同键材质原地更新，引用它的实例随之变化；未加载的键只登记，供之后的 `add` 使用。新导入的 Mesh / Texture 由 `add` 按需从磁盘读取。 |

`index` 是对象在目标列表中的最终序号，null 表示末尾。`add`/`move` 的目标列表是父级的子级，`parent` 为 null 时是根列表；`attach` 的目标列表是该 Entity 的组件列表；`move` 计数时不含被移动的对象本身。游戏生成或销毁对象会让 Play 的列表合法地不同于文档，所以超出范围的序号夹到末尾，不报诊断。同一批中的多个 `add` 按（父级，序号）升序发送；Play 的列表与文档相同时，顺序与文档一致。

数组字段按整个数组传输：经过元素路径的修改和数组的增删、移动，都扩大为对该数组字段的一次 `set`，携带完整数组（随 Scene 的数组编码一起实现）。

返回值是未完全应用的项的诊断，phase 为 `command`，消息以 `Play change <序号> #<身份>` 开头：

- warning `reference_dropped`：目标已被游戏销毁，该项跳过；或某个引用的目标已被销毁，该引用写为 null，其余照常应用。
- warning `command_rejected`：Play 无法跟随这项修改，该项不生效，例如移动 Ragdoll 部件或 Vehicle 车轮的祖先（见[物理](../physics/README.md)）。
- error `command_rejected`：应用失败，Play 已与文档分歧，需要重启 Play。`add`/`attach` 要么整项生效，要么不留下对象；`set` 在中途失败时已写入的部分不回滚。
- `add`/`attach` 携带文档本身的保留数据（占位类型、缺失资源等）时，按加载诊断的种类以 warning 报告，不算分歧。

批次格式错误，或对编辑文档调用时，整次调用返回错误。

编辑器文档一侧由 `Session::set_live_forwarding(true)` 开始记录：每次提交、撤销和重做之后，按修改留下的当前值生成上表的 op，顺序与修改一致；新增或撤销删除的多个根按（父级，序号）升序生成 `add`，绑定新材质前先生成它的 `materials` 定义。预览和取消也生成 op，同一目标（Entity 的一个 TRS 叶、一组字段、一个材质或资源键）较新的预览替换队列中较早的一项；预览过的 TRS 叶在提交或取消时都会再发送一次，即使它回到了起始值。Body 设置作为整个 `properties.body` 记录发送。选择、相机和视口设置不转发。`take_live_ops()` 取走队列中的 op。某项修改无法描述时（例如没有持久来源的程序网格），记录停止，之后的调用一直失败，直到重新设置转发：这时 Play 已经与文档分歧。

Play 进程（`run_project`）加载场景后调用 `enter_play`，UI 预览除外。`RunOptions::channel` 可以挂一个 `PlayChannel`（不持有），帧循环每帧在宿主边界调用一次它的 `service(Session&, Project&, Report)`：首帧在 start 之前，之后每次都在 update 之前。批次的取得、应用、回执都在 service 内完成，service 不阻塞也不抛出；`Report` 与 World 诊断走同一条输出（`describe_diagnostic` 逐行写入 Project Output）。编辑器启动的 Play 经 `--live-channel` 挂上链路的 Play 端；独立运行的 `--run-project` 没有 channel。

编辑器启动的 Play 由变更链路（[play_link.hpp](../../../src/application/play_link.hpp)）跟随文档：

- 传输：每次 Run 新建两条匿名管道，子进程一端的句柄只经 handle list 继承，以 `--live-channel <读> <写>` 传入。每条消息是 uint32 小端字节数加 UTF-8 JSON，单条上限 4 MiB。两端各有一个读线程和一个写线程，只搬运文本；编辑器 UI 线程和 Play 帧循环只轮询队列，不等待对方。
- 发送：Run 在序列化快照的同一步开始转发，快照与转发之间没有空隙。Play 还在加载时，批次留在管道中，在首帧 start 之前应用。编辑器每一步结束时（无论这一步从哪里返回）取走 `take_live_ops()`，作为一批发出：`{"type":"batch","seq":n,"ops":[...]}`，seq 从 1 递增。超过一条消息的批次写入本次运行的私有目录 `Cache/Play/<运行>/batch-<seq>.json`，文件内容正好是交给 `apply_live` 的 `{"ops":[...]}`，消息只带 `{"type":"batch","seq":n,"file":路径}`，Play 读完即删除。超过 16 MiB 的批次在 op 边界拆成连续的几批；单个 op 超过 16 MiB 算作无法生成。Play 结束后编辑器删除该目录。
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
- Rebuild Types 从当前场景值准备新 Session，成功才替换文档。它保留源码缓冲和源码撤销，清空依赖旧对象身份的场景撤销；存在 Undo 或 Redo 任一方向的场景历史时确认这一行为。未保存场景仍保持未保存标记。
- 删除已保存字段、改变字段类型或移除仍在场景中使用的类型时，Rebuild Types 仍然成功：旧值作为未知字段、不符值或占位对象保留，保存原样写回；恢复字段或类型后再次 Rebuild Types 即还原。只有编辑该字段或 Discard Saved Data 才丢弃旧值。
- 交互打开工程或场景时源码构建失败：仍进入工程和 Code，场景以占位对象加载（工程类型全部缺失，原生部分照常显示），可以编辑和保存，保存按原类型写回；不沿用上次成功构建的类型。构建失败期间 Run 不可用。场景对象创建后 Runtime 不再接受新类型，所以修复源码后由一次新加载恢复真实类型：没有场景 Undo/Redo 历史时自动重新加载，否则提示 Rebuild Types（清空场景历史）。严格打开在构建失败时直接拒绝。
- Cache 只放临时运行快照。关闭正常运行后删除本轮快照；源码、配置、资源和场景均保存在工程目录，删除 Cache 或搬移目录后可重建关联。

### 验收与范围

`project_integration_acceptance` 使用真实磁盘、编译器、Runtime、Workspace、UI 和独立运行进程，覆盖跨文件组件、默认值、属性投影、Scene 往返、方法热更、错误保留、跨文件定位、类型重建、字段删除/恢复与构建失败时的保留数据和占位对象、构建失败时 Play 拒绝、缺失类型时 Play 汇总、运行中 Source 故障的对象隔离与修复后恢复、子进程诊断输出与 `--halt-on-fault`、进程内运行时 Play 通道在 start 之前及每次 update 之前得到服务且批次在首帧生效、编辑器到 Play 的变更链路（批次编号与回执、超限批次落盘与按 op 拆分、error 回执、乱序回执、积压上限、链路关闭的宽限与分歧；真实 headless Play 中加载期间的修改在开始前生效，创建、撤销、重做、字段预览与取消跟随文档，Play 端读取并删除落盘批次、拒绝序号空缺且之后不再应用、对无法解析的消息报错，Stop 后清理运行目录）、项目 UI、运行隔离、游戏/UI 死循环停止、搬移工程和删除 Cache。`project_scene_acceptance` 补充真实类型的字段事务、生命周期、物理设置往返、Play 副本的变更批次（字段与名称、物理位姿与设置、World 设置、材质资产、增删与重新添加、组件附加、改父级、根与子级的序号及越界夹值、三角网格碰撞体运行中缩放后按新缩放重建、目标或引用目标已被游戏销毁、Ragdoll 无法跟随的移动、身份冲突与不分配身份），以及宽容加载的保留、写回、撤销、复制、删除目标和结构性拒绝。`editor_document_acceptance` 检查既有 UI、场景和源码编辑链。对应 Dev / Release 结果见[源码工作流验收](source-editor.md#端到端验收)。

当前提供独立运行窗口和 UI 预览。[Runtime/JIT 调试核心](../../source-debugger.md)已实现断点、单步和只读变量检查；编辑器中的交互与 Control 指令仍处于[接入方案](control.md#源码调试接入待实现)阶段。语义补全、查找替换、导出打包、运行画面嵌入编辑器及运行值回写尚未实现。

## Sponza 完整模型

[Sponza](../../../assets/examples/Examples/Sponza/README.md) 是编辑器自带默认场景，与 Scene Lit Room、Physic、Dungeons 同属 Official Examples 工程。源模板位于 `assets/examples/`，构建部署到可执行文件旁的 `examples/`；资源浏览器按 `Project / Examples / 场景名` 展示真实目录，启动时展开到 Sponza。无参数启动及顶部 Project → Examples 菜单配置内庭相机与 Scene Lit + DDGI。首次打开建立整个工程的用户副本，后续保存与切换保留修改。工程目录扫描、启动场景及 Save Scene As 同时支持 Content 和可选的 Examples 内容目录；Source 编译和 Cache 排除规则不变。部署及副本约定见 [官方 Examples](../../../assets/examples/README.md)。显式命令行的 `--project`、`--scene`、`--material-camera` 和 `--scene-lit-camera` 保留原语义。

`editor_sponza_acceptance` 运行真实依赖导入/重试/刷新/资源寿命、完整模型保存重开和正式编辑器 GPU 捕获。窗口证据在 `out/<配置>/sponza-evidence/`；基础材质像素与深度/线框检查由 `renderer_acceptance` 执行。

## Dungeons 地牢资源

[Dungeons](../../../assets/examples/Examples/Dungeons/README.md) 包含完整 Demo 和两个资源总览，使用原始世界变换生成独立 Entity。官方菜单及 `--dungeons` 配置原始 Demo 相机与 Smooth 贴图显示；无参数启动仍为 Sponza。全部非脚本 Unity 资源保留在 Originals，转换后的 2314 个模型资产可通过现有 Asset Browser 添加到场景。导入工具及不支持的 Unity 运行效果见示例说明；端到端入口为 `editor_dungeons_acceptance`。
