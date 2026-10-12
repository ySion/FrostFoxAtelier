# 项目类型、场景与运行会话

## 准备与所有权

`editor::Session::Prepare` 为 `std::function<Result<std::shared_ptr<void>>(world::World&)>`。Session 统一注册 World 和 Physics 类型并构造 World 后，在创建任何对象前调用一次。宿主在此准备项目 Source 类型及 Program、绑定生命周期，返回代码拥有者；不重复注册引擎类型。无回调的默认构造也支持物理文档；编辑器 UI 类型由独立 UI Runtime 管理。

Session 唯一保有该返回值。成员销毁次序保证 World 先于代码拥有者、Runtime 最后；Session 析构体先 close World，让 End 仍可访问代码与物理服务。显式 close 在 World 完成退出后销毁 PhysicsScene、释放历史和代码拥有者。失败反序列化同样由候选 Session 的 RAII 清理，不要求宿主维持失败对象的代码寿命。

`deserialize(text, Project&, Prepare={})` 使用同一构造入口及对象图恢复链。编辑加载与运行加载均使用它；候选成功后由应用决定发布，失败不修改原文档或文件。Source 默认字段值由 Runtime 创建真实类型时应用，Scene 不调用另一套初始化器。

## 真实类型与属性编辑

`add_actor(TypeId={})`、`add_component(TypeId)` 接受 Runtime 中真实派生类型。非空间 Component 不改变根或父子关系。`component_types()` 返回 `{TypeId type, string name}`；`properties(ObjectId)` 返回 `{string name, FieldPath path, TypeId type, Scalar value}`。显示名称不充当身份；写入用实际 FieldId 路径。

Actor 和 Component 的标量属性使用相同的 `begin_edit / preview_edit(Scalar) / commit_edit / cancel_edit` 和 undo/redo。float 重载供现有 Position 消费者继续使用。TRS 编辑保存完整向量组，其他属性保存精确 Scalar；错误类型、非有限数和非成员对象不能进入编辑。移除对象仍可由历史保活，但保活不授予文档成员资格。

发现和持久化支持公开的 bool、float、double、int、uint、枚举，以及由这些字段组成的内联 record。隐藏关系继续由 World 管理；业务对象引用、String/Array 等尚未有此格式编码的字段明确报错，不静默遗漏。枚举保存成员名，不保存其 Runtime TypeId。

## Scene 格式

统一写出版本 4，继续读取版本 1/2/3；其他版本拒绝。旧版本保存时升级为 v4，已有类型、TRS、组成、网格来源和材质保留。

每个 Actor/Component 保存完整类型名称，并使用 Runtime `find_type / is_a` 校验。每个对象有 `properties` 对象，按声明字段名递归保存业务标量；新增缺省字段保留 Runtime 构造默认值，未知已保存属性报错。整数严格检查范围和符号，不做环绕转换；浮点必须有限。

SceneComponent 的 position/rotation/scale/parent、StaticMeshComponent 的 mesh/base_color/roughness/metallic/emissive 走专有验证及资源恢复，不在 properties 重复保存。旧场景缺少新增材质字段时采用 World 的 Material 默认值；加载和保存均调用 World 材质验证。组件 ID 仅为文件内该 Actor 的局部身份；组件顺序、显式根、同 Actor 空间关系保持原合同，文件不记录 Runtime TypeId、ObjectId 或 FieldId。

StaticMeshComponent 的 `materials` 保存命名槽的资源 GUID 和显式赋予标志。参数实例额外保存父材质 GUID、覆盖字段名称和参数值，加载后重新建立 Runtime 引用，不按与父值是否相等猜测覆盖。共享材质资产由 Project 单独持久化，Scene 保存引用；未保存材质由编辑器单独提示和保存。复制 Actor 时复制其场景内联材质和参数实例，共享父资产与 Texture；换网格时移除失配槽。

LightComponent 沿普通空间组件记录类型、TRS、根及父关系；LightSettings 通过通用 `properties.settings` 递归保存，kind 保存枚举成员名。加载先恢复全部字段，再调用 World 的灯参数验证，避免字段恢复顺序改变有效性；保存同样拒绝非法设置。Create 菜单的三种灯只生成一个普通 Actor 和一个作为根的 LightComponent，整组加入文档历史；Actor Structure 的通用增删入口也可操作该组件。没有第二套灯存档或独立灯管理器。

`PrimitiveComponent` 的 `body` 是生成反射的 BodySettings 值，StaticMeshComponent 与 CollisionComponent 共享。启用、形状、运动模式、尺寸、类别/响应掩码和物理材料沿通用 `properties.body` 保存并由相同字段路径编辑；Source/JIT 读写同一内存。保存与加载调用 World 的配置验证，不写入 Jolt 身份或运行态。

加载先恢复全部网格、TRS 和空间关系，再按每个 Primitive 的世界缩放读取工程三角网格 Cook 缓存。内置网格使用 CPU 几何；Cook 字节是派生缓存，不写入 Scene。旧 v1/v2 BodyComponent 在加载时合并到根 Primitive；普通 SceneComponent 根升级为 CollisionComponent。旧名称只存在于单向文档迁移，运行时不注册旧类，保存只输出反射配置。自定义非 Primitive 根或多个旧 Body 无法无损合并时明确报错。`Session::configure_body` 的整值事务与 Parameters 字段编辑修改同一对象；Actor Structure 添加独立碰撞时使用 CollisionComponent。

加载使用真实类型的低层 create，再恢复字段/资源/组成，不调用会额外生成默认根的 StaticMeshActor 工厂。未找到类型或资源、属性不支持、组成或空间校验失败时，整个候选丢弃，原文件保持原样。

## 运行驱动

`Session::start/update/stop` 是运行宿主的唯一驱动。start 创建 PhysicsScene，再开始 World；未运行前不创建物理场景。运行中 update 通过 PhysicsScene::update 执行一次游戏 Tick 和物理子步；没有物理场景时沿 World 原有准入处理。任一路径错误都执行停止清理；重复 stop 不重复 End。stop/close 在 World 完成 End 后释放物理场景。

准备好的行为代码存活到 close；停止后可重新 start。应用可以为每次 Play 从编辑场景快照创建新 Session，编辑文档和原撤销历史不参与运行，也不把运行结果自动写回。运行数据不得复用编辑 Runtime 的身份。

End 回调内同步调用 Session::stop 会沿 World 准入返回 busy；被拒绝的调用不释放 PhysicsScene。物理服务保留到外层 World 退出完成、物理 update 返回后，再由宿主释放。

## 验收

`project_scene_acceptance` 使用真实 Runtime 原生派生类型、Source/JIT 调用、Session、Scene、资源目录和 Jolt，覆盖默认值、类型/字段发现、Actor/Component 标量事务、失败加载释放、v1/v2、独立运行身份、反复启停、Begin/Tick/End 错误和物理争写拒绝。原 `world_acceptance` 继续覆盖顺序生命周期与对象图边界。新的多文件 Source class 发布链由项目编译集成验收补齐，不以原生派生类型消费者替代其证据。

## World authoring 配置

Scene v4 根 `world` 保存共享 `rendering::SceneLightingSettings`。它由 World 持有，Scene 编解码和 Session 历史负责持久化与撤销；Renderer 只消费副本。v1–v3 使用类型默认值。诊断偏好、GPU 缓存和实际 follow-focus 锚点不属于存档，完整规则见[持久格式与责任](../editor/projects.md#持久格式与责任)。
