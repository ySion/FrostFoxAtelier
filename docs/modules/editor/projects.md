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

## 资源浏览器

原 Asset Shelf 沿原停靠视图演进为 Asset Browser。左侧常驻目录树以 Project、Engine 为并列根：Project 展示实际 Content、Source、Config 及可选的 Examples 目录，Engine 展示只读内置网格；未打开工程时 Project 根禁用。路径与搜索位于同一紧凑工具行，路径菜单可访问祖先目录，返回和上级操作使用同一导航状态。类型筛选、List/Details/Tiles 和资源操作各由图标菜单承载。

单击资源只改变浏览器选择，不修改 Scene Tree 选择或创建 Actor。更多菜单及资源右键菜单中的 Open 进入目录或打开场景，Details 的双击/Enter 走同一 Open 入口。Add to Scene 执行既有 StaticMeshActor 创建命令；Import OBJ、Refresh 也沿原命令入口。右键先选中当前资源，再构建菜单；原资源消失或选择改变后关闭旧菜单。切换视图保留所选资源、过滤、排序和列宽。Tiles 使用类型图标，不声称已经实现模型缩略图。

查询或类型过滤排除当前资源时立即清空选择，并禁用对应 Open/Add；仍匹配的选择保留。清除过滤不恢复先前的隐藏选择。目录导航使用共享 Tree 的展开、选择与键盘行为；从路径或内容进入目录时显露对应树行。视图和类型菜单使用互斥单选标记。目录刷新淘汰失效导航记录，当前目录消失时回到有效来源根。

Details 使用共享 Table 的 Name / Type / Size 列，列头显示排序方向；Size 按原始字节数排序、右对齐，目录始终排在文件前。列边界拖动有最小宽度，松手保存本次列宽，Esc/失焦/捕获丢失取消。窄面板保留各列并提供横向滚动。整表是一个 Tab 停靠点，方向键、Home/End/PageUp/PageDown 移动当前单元格并显露所选行。排序保持资源身份；选择变化复用原数据快照。详细控件合同见[共享表格](../ui/README.md#表格)。

工具行、目录树、内容槽和状态行由 `AssetBrowserShell` 声明，List/Details/Tiles 均由 `AssetResults` 的同一个 Table 节点声明，均位于 [editor.ui.source](../../../assets/editor.ui.source)。保存可更新这些区域的布局，同时保留兼容的控件身份、选择、排序列宽和搜索草稿。浏览器只提供目录/资源快照和既有命令；目录模板按实际变化缓存。全部结果由共享 Table 按当前视图虚拟绘制，原浏览器虚拟列表、合成资源按钮与逐项菜单选择路径已移除。

Import OBJ 导入 Wavefront OBJ 的位置、法线、UV、MTL 漫反射材质及依赖贴图。简单多边形（每面最多 64 个角点）按投影耳切三角化，保留原边界 mask；支持正/负索引，缺少法线时生成面法线，带贴图的面必须提供有效 UV。OBJ 的 V 轴转换到图片左上原点，超出 [0,1] 的 UV 保留并重复采样。单独 OBJ 保持 `Content/Meshes/<文件名>` 的位置；带依赖的模型使用 `Content/Meshes/<模型名>/` 独立目录，保持相对依赖关系。新增带材质的 Actor 使用白色实例颜色，避免默认蓝色染色。

MTL 支持 `Kd`、`d`/`Tr`、`map_Kd` 与 `map_d`。WIC 解码 PNG/JPEG/BMP 等系统支持格式；颜色贴图按 sRGB 采样，遮罩按线性值读取。其他常见 MTL 光照参数及贴图保留源文件但不参与当前着色；尚无法线/高度贴图、PBR、半透明混合、动态阴影或 DDGI。带 `-s`/`-o` 等选项的 map 明确报错，动画仍未导入。

依赖只能位于 OBJ 所在目录及子目录；绝对路径、上级路径、逃逸链接、缺失文件与非法数据在发布前拒绝。单文件最多 128 MiB、整包最多 512 MiB、解码像素最多 768 MiB，图片单边最多 8192；最多 512 个依赖文件、1024 个命名材质、100 万个位置/法线/UV 和 100 万个三角形。导入验证和写盘使用相同字节快照，先检查现有目录，再独占创建依赖、网格与身份文件；已有同名文件不覆盖。发布后目录检查失败时仅删除本轮创建的文件，允许修复后重试；空目录可能保留，清理失败明确报告。

Refresh/F5 重新扫描目录，不在 UI 每帧访问磁盘。网格 UUID 与依赖文件的大小/修改时间共同决定缓存是否有效；未变化刷新保留 revision 和资源，MTL/图片改写或删除淘汰相应网格解析缓存。已有 Actor 保有旧 Mesh/图片版本，重新打开场景或再次添加资源取得新版本。外部工具应正常更新文件修改时间；刻意同时保持大小和时间的内容改写无法识别。


每个 OBJ 旁的 `<文件名>.asset.json` 保存版本和持久 UUID。移动、重命名资源时须一同移动该元数据，并保持 MTL/贴图的相对引用；复制资源应使用 Import，以获得独立身份。Refresh 为新增 OBJ 建立元数据，重复 ID 或非法元数据明确报错。元数据与资源共同位于 Content 或 Examples，需要与源文件一同保存到版本控制；Cache 不承担资源身份。目录索引首片最多 10000 项。查询结果按目录版本缓存；List/Details/Tiles 共用 Table 的可见单元格绘制，不为整个目录建立布局节点。

## 持久格式与责任

- Project 和资源身份文件为 UTF-8 JSON、`version: 1`；Scene 写出 `version: 2`，继续读取 v1。JSON 解析由锁定的 nlohmann/json 提供。共用内部格式校验：16 MiB 上限、最多 32 层容器、禁止重复字段和未知字段、身份及版本必须是非负整数、向量必须精确匹配长度且各项有限。内部 JSON 结构不穿过 Project/Session 公共接口。
- Project 保存名称、相对 `startup_scene` 和可选的 `ui_source`（Source 内的 `.ui.source` 入口）。目录解析检查绝对路径、上级路径和解析后逃出工程的链接，不以进程当前目录寻找资源。
- Scene 保存有序 Actor、真实类型名称、名称、enabled、有序组件、局部组件 ID、显式 root/parent、TRS、材质和资源键。存档不写 Runtime 的 TypeId/ObjectId/FieldId、内存地址或 GPU 资源。
- TRS 按 `position` / `scale` 的 xyz 数组和 `rotation` 的 xyzw 数组保存，颜色同样是数值数组；不保存数学类型名或原生内存布局。统一到 `math.vector3` / `math.quaternion` 后，float 精度、分量顺序、局部变换与行向量组合语义保持一致，因此 Scene v1 继续可读，无需版本迁移或旧类型别名。
- Scene 按限定类型名恢复已注册的 Actor/Component 派生类型；公开标量和内联记录进入 `properties`，新增未保存字段保留 Runtime 默认值。Physics Body 设置由专有字段保存。未知版本、未知类型、未知已保存属性、失效关系、缺失资源和无法编码的字段明确失败；空网格保留为空。单场景最多 256 Actor、每个 Actor 最多 1024 组件、文件最多 16 MiB。详见[项目场景合同](../world/project-scenes.md)。
- StaticMeshComponent 自身保存反射可见的 `mesh_source`；World 的 `set_mesh_binding` 一次替换来源和解析内容，`read_static_mesh` 覆盖 Detached 与空网格。裸 `set_geometry` 清空旧来源。编辑器不维护 Component ID 到资源键的权威副表。
- 重建先创建真实类型对象和组件，恢复值，每个 Actor 一次 `attach_components` 恢复组成与空间关系，最后按文档顺序 attach。加载 StaticMeshActor 不重复调用带默认组成的快捷工厂。
- 保存先完成有效文本草稿，经同目录临时文件、FlushFileBuffers 和替换发布。写入失败保留旧文件；这不是跨文件事务或断电恢复保证。工程标记最后创建，未完成创建不会伪装成可打开工程。存档提交与目录刷新结果分开：成功保存后即更新保存点，资源刷新失败显示警告，不将成功写入误报为保存失败。
- 场景撤销回到保存位置后恢复干净状态；历史分叉或裁剪使旧保存位置不可达时保持未保存状态。浏览、过滤和面板布局不进入场景历史。

## 验收

`editor_document_acceptance` 还直接运行真实 Project → Workspace → UI 布局/绘制/命中链，在两种逻辑窗口尺寸与 1/1.5/2 DPI 下切换三种视图和滚动选择，不创建桌面窗口。文件链覆盖真实目录创建/打开、中文名、OBJ 导入、元数据身份、组件和空网格往返、保存点撤销、坏版本/坏 JSON、写入失败、资源改名、工程搬移、Cache 删除及无来源网格拒绝保存。

同一验收还覆盖非法数值身份、向量长度、重复键、过深/超限输入、独占发布、坏目录导致的导入重试，以及 10000 项目录导致发布后失败时的文件回滚。刷新无变化、文件改名和真实内容改写分别验证缓存复用、旧版本生命周期与新版本解析。

固定 Scene v1 文件独立于当前序列化器，经过真实读盘、Session/World 重建、父子矩阵计算、资源解析、保存与重开，核验统一数学类型后的格式兼容性。

普通 `editor_acceptance` 通过同一应用检查工程命令、Ctrl+S 提交字段、项目重开、三种浏览视图、搜索和类型过滤、资源选择与场景选择隔离、导入定位、坏场景保留当前文档及工程搬移。每轮使用独立证据目录。

## 性能测量

`out/release/atelier_editor_document_acceptance.exe --profile-json <新证据目录>` 使用现有消费者创建独立工程，测量 1/64/256 Actor，以及 256 Actor、每个 16 组件的真实存档。每项预热 2 次、采样 21 次，记录文件字节数、中位数、P95 与最大值。分别测量暖文件缓存读取、原始 DOM 解析、含输入边界检查的解析、DOM 输出、场景序列化、从文本重建和读盘重建。DOM 与文档销毁在计时外，不包含窗口、GPU 或磁盘冷读；没有测量解析器独占内存峰值。测量目录不得指向用户实用工程。

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
2. 编辑后 Ctrl+S。F6 或 Project → Build 编译所有工程 Source；外部保存由 500 ms 文件检查发现。新增类型、字段、默认值或改变公开函数签名后，停止运行并选择 Rebuild Types。
3. 选择一个 Actor，打开 Window → Actor Structure，通过 Add Component 选择 `project.Move`。添加后自动检查该组件。Parameters 中公开数值使用单行参数编辑器，bool 使用 Checkbox，枚举使用命名选项菜单；结构页负责删除，属性修改与结构变化都进入场景文档。
4. 保存场景。F8 或 Run 启动独立游戏窗口；Shift+F8 或 Stop 停止。Project Output 显示构建、运行与退出信息。
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

`translate_owner` 将位移请求交给 World，在回调边界应用。拥有 BodyComponent 的 Actor 由物理控制，此操作会被拒绝；不绕过物理变换所有权。语言声明、原生派生和字段默认值的精确范围见[Source 类型声明](../../source-type-declarations.md)。

### 加载与执行链

```text
Project 配置 + Source 文件快照
  → Session::Prepare 注册原生 World / Physics 类型
  → ProjectCode → Source Program 准备并发布项目类型、行为
  → Scene v2 解析类型名、属性、资源和关系
  → 停止的编辑文档
  → Run：序列化当前场景到 Cache/Play，启动同一可执行文件的运行入口
  → 独立 Session → start → update → stop
```

Source 目录递归收集 `.source` 文件，排除 `.ui.source`，按相对路径排序。每份文件保留正文和版本；上限为 256 份、单份 1 MiB、总计 16 MiB。源码编译是同步、整工程候选发布；没有逐文件混合发布或磁盘机器码缓存。未改变内容不会重复编译。轮询依赖修改时间和大小，显式 Build 会重新读取内容。

Session 在创建对象之前调用 Prepare，独占保有返回的代码拥有者。退出顺序保证 End 能访问行为代码，World 先于代码释放，Runtime 最后销毁。启用 PhysicsScene 后只调用其 update，由它更新 World，避免重复 Tick。编辑器 UI Runtime 由 Workspace 独立拥有，场景替换不会让 UI 借用失效的运行时。

项目 Play 和 UI Preview 共用一个进程宿主。父进程只传入工程位置、场景快照、停止事件并读取输出。Stop 先请求正常退出；2 秒未响应后结束该运行进程组。关闭编辑器或切换工程也回收它。运行中的对象身份、GPU 资源和 JIT 地址不会跨进程传递。停止后保留编辑场景、选择和撤销历史；运行结果不会自动写回。

命令行与编辑器使用同一个入口：

```powershell
atelier_editor --run-project E:\ffa_testproj
atelier_editor --run-project E:\ffa_testproj --preview-ui
```

### 项目 UI

Project → New Project UI 创建 `.ui.source` 并写入 `Config/Project.json` 的 `ui_source`。Edit Project UI 打开该文件，Preview Project UI 使用独立预览窗口；游戏窗口也加载同一个入口：

```cpp
ui ProjectUI(in Text title, in double elapsed, in double actors) {
    Column hud {
        padding_left: 8; padding_top: 8; gap: 4;
        Text title { text: input.title; }
        Text hint { text: "Edit this UI in your project."; }
    }
}
```

三个输入分别为工程名、运行秒数和 Actor 数量。项目 UI 复用 Source/JIT、LiveDocument、原生 UI、SS、字体、输入和 renderer。预览不启动游戏行为；游戏中 UI 与场景分别拥有 Runtime，只交换输入值。

编辑器对项目 UI 只做编译检查，不求值表达式和事件。用户 UI 代码在子进程执行，所以表达式或事件陷入死循环也能停止。保存兼容 UI 变更保留局部状态。ProjectUI::prepare 统一完成源码、控件和真实布局准备，成功后才发布候选；错误恢复上次成功定义、状态与控件。窗口最小化或零 Surface 时暂停发布；冷启动无有效版本时报告失败。入口配置变更在下一次启动运行或预览时采用。

File → Edit Editor UI Source 是引擎开发命令，用于修改编辑器自身的受信任 UI；它与项目 UI 的执行边界不同。SS 仍按启动加载策略生效。

### 保存、失败与重建

- 未保存源码保留在 CodeDocument 中；构建、运行和预览命令先保存当前工程的已修改 Source 文档，磁盘冲突按原保存流程处理。
- 方法体更新保留对象、字段和场景历史；稳定 Entry 在下一次调用使用当前代码，旧 Lease 保留其代码。构建失败不替换最后成功版本。
- Rebuild Types 从当前场景值准备新 Session，成功才替换文档。它保留源码缓冲和源码撤销，清空依赖旧对象身份的场景撤销；存在 Undo 或 Redo 任一方向的场景历史时确认这一行为。未保存场景仍保持未保存标记。
- 删除已保存字段、改变字段类型或移除仍在场景中使用的类型会明确失败。先用旧版本移除相应组件或迁移数据，不静默丢值。
- 初次打开时源码失败仍进入工程和 Code；原场景文本保留，创建和保存场景被禁用。修复源码后重建候选再激活场景，不能用空白替代文档覆盖原文件。
- Cache 只放临时运行快照。关闭正常运行后删除本轮快照；源码、配置、资源和场景均保存在工程目录，删除 Cache 或搬移目录后可重建关联。

### 验收与范围

`project_integration_acceptance` 使用真实磁盘、编译器、Runtime、Workspace、UI 和独立运行进程，覆盖跨文件组件、默认值、属性投影、Scene 往返、方法热更、错误保留、跨文件定位、类型重建、项目 UI、运行隔离、游戏/UI 死循环停止、搬移工程和删除 Cache。`project_scene_acceptance` 补充真实类型的字段事务、生命周期和物理设置往返。`editor_document_acceptance` 检查既有 UI、场景和源码编辑链。对应 Dev / Release 结果见[源码工作流验收](source-editor.md#端到端验收)。

当前提供独立运行窗口和 UI 预览。断点、单步、语义补全、查找替换、导出打包、运行画面嵌入编辑器及运行值回写不在本次实现范围。

## Sponza 完整模型

[Sponza](../../../assets/examples/Examples/Sponza/README.md) 是编辑器自带默认场景，与 Scene Lit Room 同属 Official Examples 工程。源模板位于 `assets/examples/`，构建部署到可执行文件旁的 `examples/`；资源浏览器按 `Project / Examples / 场景名` 展示真实目录，启动时展开到 Sponza。无参数启动及顶部 Project → Examples 菜单配置内庭相机与 Scene Lit + DDGI。首次打开建立整个工程的用户副本，后续保存与切换保留修改。工程目录扫描、启动场景及 Save Scene As 同时支持 Content 和可选的 Examples 内容目录；Source 编译和 Cache 排除规则不变。部署及副本约定见 [官方 Examples](../../../assets/examples/README.md)。显式命令行的 `--project`、`--scene`、`--material-camera` 和 `--scene-lit-camera` 保留原语义。

`editor_sponza_acceptance` 运行真实依赖导入/重试/刷新/资源寿命、完整模型保存重开和正式编辑器 GPU 捕获。窗口证据在 `out/<配置>/sponza-evidence/`；基础材质像素与深度/线框检查由 `renderer_acceptance` 执行。
