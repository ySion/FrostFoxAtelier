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
- Ctrl+S 保存当前场景，Ctrl+Shift+S 另存到当前工程 Content 内，File → Open Scene 打开同一工程中的场景。标题显示工程、场景和未保存标记。
- 关闭窗口、打开其他工程或场景前处理未保存修改。保存决定完成后才读取目标场景，取消新建也不会创建目录。加载在独立、停止的新 Session/World 中完成；失败保留当前文档，成功才撤下旧 UI 绑定和文档。
- Config 的 `startup_scene` 是打开工程时加载的场景，路径相对工程根。另存场景不隐式修改启动场景配置。
- Source 参与目录浏览；本片没有新增跨文件语言工程编译或运行入口，继续由正式语言设施演进。

## 资源浏览器

原 Asset Shelf 沿原停靠视图演进为 Asset Browser。Project 展示实际 Content、Source、Config 目录；Engine 展示只读内置网格。目录树显示来源根和当前目录链，内容区浏览子目录。当前路径、名称搜索、类型下拉、清除过滤、List/Details/Tiles 共用同一来源与选择。

单击资源只改变浏览器选择，不修改 Scene Tree 选择或创建 Actor。Open 进入目录或打开场景；Add to Scene 执行既有 StaticMeshActor 创建命令。切换视图保留所选资源和过滤。Tiles 使用类型图标，不声称已经实现模型缩略图。

Import OBJ 导入三角化的 Wavefront OBJ 几何到 Content/Meshes；支持正/负位置索引、可选顶点法线，缺少法线时生成面法线。当前不导入 UV、MTL 材质、纹理或动画，面必须已三角化；不支持的几何记录报错。导入使用同一份源字节完成验证和写入，先检查现有目录，再独占创建网格与身份文件；同名文件或残留身份文件不覆盖。发布后目录检查失败时，仅回滚本轮创建的文件，允许修复原因后重试；回滚本身失败会明确报告。

Refresh/F5 重新扫描目录并更新目录索引；不在 UI 每帧访问磁盘。未变化目录保留 revision，网格按 UUID、文件大小和修改时间保留缓存，资源改名也继续共享相同 MeshRef。变化或消失的资源才淘汰解析缓存；已有 Actor 持有的旧几何版本保持有效，不自动替换。外部工具应正常更新文件修改时间；刻意同时保持大小和修改时间的内容改写无法通过该检查识别。

每个 OBJ 旁的 `<文件名>.asset.json` 保存版本和持久 UUID。移动、重命名资源时须一同移动该元数据；复制资源应使用 Import，以获得独立身份。Refresh 为新增 OBJ 建立元数据，重复 ID 或非法元数据明确报错。元数据属于 Content，需要与源文件一同保存到版本控制；Cache 不承担资源身份。目录索引首片最多 10000 项。查询结果按目录版本缓存；列表、详情和图块复用既有 VirtualList，仅描述可见行及缓冲区域，避免把整个目录转成控件。

## 持久格式与责任

- Project、Scene 和资源身份文件均为 UTF-8 JSON、`version: 1`；JSON 解析由锁定的 nlohmann/json 提供。共用内部格式校验：16 MiB 上限、最多 32 层容器、禁止重复字段和未知字段、身份及版本必须是非负整数、向量必须精确匹配长度且各项有限。内部 JSON 结构不穿过 Project/Session 公共接口。
- Project 保存名称及相对 `startup_scene`。目录解析检查绝对路径、上级路径和解析后逃出工程的链接，不以进程当前目录寻找资源。
- Scene 保存有序 Actor、真实类型名称、名称、enabled、有序组件、局部组件 ID、显式 root/parent、TRS、材质和资源键。存档不写 Runtime 的 TypeId/ObjectId/FieldId、内存地址或 GPU 资源。
- 首版支持现有 Actor/StaticMeshActor 和 Component/SceneComponent/StaticMeshComponent。未知版本、未知类型、失效关系、缺失资源或不能完整保存的程序化网格明确失败；空网格保留为空。单场景最多 256 Actor、每个 Actor 最多 1024 组件、文件最多 16 MiB。
- StaticMeshComponent 自身保存反射可见的 `mesh_source`；World 的 `set_mesh_binding` 一次替换来源和解析内容，`read_static_mesh` 覆盖 Detached 与空网格。裸 `set_geometry` 清空旧来源。编辑器不维护 Component ID 到资源键的权威副表。
- 重建先创建真实类型对象和组件，恢复值，每个 Actor 一次 `attach_components` 恢复组成与空间关系，最后按文档顺序 attach。加载 StaticMeshActor 不重复调用带默认组成的快捷工厂。
- 保存先完成有效文本草稿，经同目录临时文件、FlushFileBuffers 和替换发布。写入失败保留旧文件；这不是跨文件事务或断电恢复保证。工程标记最后创建，未完成创建不会伪装成可打开工程。存档提交与目录刷新结果分开：成功保存后即更新保存点，资源刷新失败显示警告，不将成功写入误报为保存失败。
- 场景撤销回到保存位置后恢复干净状态；历史分叉或裁剪使旧保存位置不可达时保持未保存状态。浏览、过滤和面板布局不进入场景历史。

## 验收

`editor_document_acceptance` 还直接运行真实 Project → Workspace → UI 布局/绘制/命中链，在两种逻辑窗口尺寸与 1/1.5/2 DPI 下切换三种视图和滚动选择，不创建桌面窗口。文件链覆盖真实目录创建/打开、中文名、OBJ 导入、元数据身份、组件和空网格往返、保存点撤销、坏版本/坏 JSON、写入失败、资源改名、工程搬移、Cache 删除及无来源网格拒绝保存。

同一验收还覆盖非法数值身份、向量长度、重复键、过深/超限输入、独占发布、坏目录导致的导入重试，以及 10000 项目录导致发布后失败时的文件回滚。刷新无变化、文件改名和真实内容改写分别验证缓存复用、旧版本生命周期与新版本解析。

普通 `editor_acceptance` 通过同一应用检查工程命令、Ctrl+S 提交字段、项目重开、三种浏览视图、搜索和类型过滤、资源选择与场景选择隔离、导入定位、坏场景保留当前文档及工程搬移。每轮使用独立证据目录。

## 性能测量

`out/release/atelier_editor_document_acceptance.exe --profile-json <新证据目录>` 使用现有消费者创建独立工程，测量 1/64/256 Actor，以及 256 Actor、每个 16 组件的真实存档。每项预热 2 次、采样 21 次，记录文件字节数、中位数、P95 与最大值。分别测量暖文件缓存读取、原始 DOM 解析、含输入边界检查的解析、DOM 输出、场景序列化、从文本重建和读盘重建。DOM 与文档销毁在计时外，不包含窗口、GPU 或磁盘冷读；没有测量解析器独占内存峰值。测量目录不得指向用户实用工程。

JSON 当前位于打开、保存和显式目录刷新路径；帧绘制不解析 JSON。尚未建立不同解析库的同条件比较，不能据此宣称极限吞吐。优化取舍应同时看格式解析和真实 World 重建成本。

## 设计依据

- [Houdini 项目管理](https://www.sidefx.com/docs/houdini/basics/project.html)：目录范围与场景加载职责分开，使用相对项目位置的资源引用。
- [Godot 文件系统](https://docs.godotengine.org/en/stable/tutorials/scripting/filesystem.html)：项目根目录和资源路径形成可搬移的工作范围。本项目按用户明确方向使用 Config/Content/Source/Cache。
- [Windows 文件资源管理器](https://support.microsoft.com/en-us/accessibility/windows/use-a-screen-reader-to-explore-and-navigate-file-explorer-in-windows)：目录导航、内容选择、详情及视图切换分别承载清楚的任务。
- [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw) 与 [JSON 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0)：当前文件替换机制与固定依赖来源。
