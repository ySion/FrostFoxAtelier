# 项目类型、场景与运行会话

## 准备与所有权

`editor::Session::Prepare` 为 `std::function<Result<std::shared_ptr<void>>(world::World&)>`。Session 注册 World 类型并构造 World 后，在创建任何对象前调用一次。宿主在此注册需要的 Physics 类型、准备项目 Source 类型及 Program、绑定生命周期，返回代码拥有者。无回调的默认构造仍支持纯 World 文档；编辑器 UI 类型由独立 UI Runtime 管理。

Session 唯一保有该返回值。成员销毁次序保证 World 先于代码拥有者、Runtime 最后；Session 析构体先 close World，让 End 仍可访问代码与物理服务。显式 close 在 World 完成退出后销毁 PhysicsScene、释放历史和代码拥有者。失败反序列化同样由候选 Session 的 RAII 清理，不要求宿主维持失败对象的代码寿命。

`deserialize(text, Project&, Prepare={})` 使用同一构造入口及对象图恢复链。编辑加载与运行加载均使用它；候选成功后由应用决定发布，失败不修改原文档或文件。Source 默认字段值由 Runtime 创建真实类型时应用，Scene 不调用另一套初始化器。

## 真实类型与属性编辑

`add_actor(TypeId={})`、`add_component(TypeId)` 接受 Runtime 中真实派生类型。非空间 Component 不改变根或父子关系。`component_types()` 返回 `{TypeId type, string name}`；`properties(ObjectId)` 返回 `{string name, FieldPath path, TypeId type, Scalar value}`。显示名称不充当身份；写入用实际 FieldId 路径。

Actor 和 Component 的标量属性使用相同的 `begin_edit / preview_edit(Scalar) / commit_edit / cancel_edit` 和 undo/redo。float 重载供现有 Position 消费者继续使用。TRS 编辑保存完整向量组，其他属性保存精确 Scalar；错误类型、非有限数和非成员对象不能进入编辑。移除对象仍可由历史保活，但保活不授予文档成员资格。

发现和持久化支持公开的 bool、float、double、int、uint、枚举，以及由这些字段组成的内联 record。隐藏关系继续由 World 管理；业务对象引用、String/Array 等尚未有此格式编码的字段明确报错，不静默遗漏。枚举保存成员名，不保存其 Runtime TypeId。

## Scene 格式

统一写出版本 2，继续读取版本 1；其他版本拒绝。v1 保存时升级为 v2，已有类型、TRS、组成、网格来源和材质保留。

每个 Actor/Component 保存完整类型名称，并使用 Runtime `find_type / is_a` 校验。每个对象有 `properties` 对象，按声明字段名递归保存业务标量；新增缺省字段保留 Runtime 构造默认值，未知已保存属性报错。整数严格检查范围和符号，不做环绕转换；浮点必须有限。

SceneComponent 的 position/rotation/scale/parent、StaticMeshComponent 的 mesh/base_color 仍走专有验证及资源恢复，不在 properties 重复保存。组件 ID 仅为文件内该 Actor 的局部身份；组件顺序、显式根、同 Actor 空间关系保持原合同，文件不记录 Runtime TypeId、ObjectId 或 FieldId。

`physics.BodyComponent` 通过 `body` 专有记录保存 shape（box/sphere/triangle_mesh）、motion（stationary/kinematic/dynamic）、half_extents 和 radius；读取走公开 `physics::configure`，写出使用 `read_settings`。这些 settings 目前未反射，因此不假装由通用 properties 编辑。Physics 类型由宿主 Prepare 注册，Scene 不临时补注册已封闭的类型。

加载使用真实类型的低层 create，再恢复字段/资源/组成，不调用会额外生成默认根的 StaticMeshActor 工厂。未找到类型或资源、属性不支持、组成或空间校验失败时，整个候选丢弃，原文件保持原样。

## 运行驱动

`Session::start/update/stop` 是运行宿主的唯一驱动。start 在存在注册的 physics.BodyComponent 类型时创建 PhysicsScene，再开始 World；未运行前不创建物理场景。update 选择 PhysicsScene::update 或 World::update，只执行一次游戏 Tick。任一路径错误都执行停止清理；重复 stop 不重复 End。stop/close 在 World 完成 End 后释放物理场景。

准备好的行为代码存活到 close；停止后可重新 start。应用可以为每次 Play 从编辑场景快照创建新 Session，编辑文档和原撤销历史不参与运行，也不把运行结果自动写回。运行数据不得复用编辑 Runtime 的身份。

## 验收

`project_scene_acceptance` 使用真实 Runtime 原生派生类型、Source/JIT 调用、Session、Scene、资源目录和 Jolt，覆盖默认值、类型/字段发现、Actor/Component 标量事务、失败加载释放、v1/v2、独立运行身份、反复启停、Begin/Tick/End 错误和物理争写拒绝。原 `world_acceptance` 继续覆盖顺序生命周期与对象图边界。新的多文件 Source class 发布链由项目编译集成验收补齐，不以原生派生类型消费者替代其证据。
