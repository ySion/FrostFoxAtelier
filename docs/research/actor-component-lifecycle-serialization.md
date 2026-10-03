# Actor/Component 生命周期与场景归档

状态：2026-10-02 生命周期部分已落实到当前工作树；序列化部分仍是后续设计提案。用户先授权实现生命周期，撤销事务暂缓，原有编辑器行为继续按当前合同工作。实际生命周期与验收入口以 [World 当前合同](../modules/world/README.md) 为准；[此前协调记录](object-serialization-transactions.md)只作后续参考。

## 现有基础与真实缺口

- Actor 与 Component 已共同派生 Runtime 的 `core.Object` 语义根；SceneComponent 和 StaticMeshComponent 承载空间与网格能力。类型定义由头文件反射工具或语言定义生产者发布到同一 Runtime。[World 合同](../modules/world/README.md)
- Actor 与组件 Begin/Tick/End 已按成员与组成顺序共用同一执行链，End 反向；成功 Begin 才配对 End，enabled 只控制 Tick 并取消相关任务。
- 对象构造、World 退出与最终销毁已有入口；尚无完整的加载准备、加载完成和通用文件归档协议。
- `World::attach_components` 已接受同 Runtime 的 Detached Actor；先建立完整组成，再由 attach 注册 World，复用同一关系校验。Detached 图同时可通过现有入口配置、换父、移除组件和销毁。[现有入口](../../src/world/world.cpp)

## 生命周期分工

| 阶段 | 建议合同 |
| --- | --- |
| 构造 | Runtime 建立对象身份与有效默认存储；原生构造不负责 World 注册或 Begin |
| 数据准备 | 新建对象配置数据；文件加载填充声明字段、解析对象和资源引用 |
| 组成完成 | 建立 Actor 有序组件集合、根与空间树；完整校验成功后对象才进入可注册状态 |
| 数据就绪 | 新建与加载携带明确来源，按需通知；回调可以看到完整已解析图，尚未参与游戏运行 |
| 注册 World | 建立成员、查询及呈现关系；停止状态也可呈现，注册不自动等同于 Begin |
| 开始与更新 | start 或进入运行中的 World 时 Begin；Tick 遵守已确定的顺序与启用资格 |
| 结束与注销 | stop 只结束运行；离场先取消任务、End，再撤参与关系；成员注销与组件脱离 Actor 分别表达 |
| 最终销毁 | World 游戏对象终结入口先处理关系与运行退出，再调用 Runtime 销毁；GC 与原生析构不代替 End |

普通 stop 后可再次 start，并进入下一次运行周期。close 结束运行并关闭 World 服务，不隐式销毁仍被外部持有的对象。Begin、Tick、End 失败处理沿现有合同，不承诺回滚业务回调的外部副作用。

UE 可参考的分工是 UObject 构造后的初始化、PostLoad、组件注册与呈现状态，以及 Actor 的 BeginPlay/EndPlay 和最终 GC；具体钩子数量与命名按本项目需要确定。[UObject](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UObject)、[组件注册](https://dev.epicgames.com/documentation/en-us/unreal-engine/components-in-unreal-engine)、[Actor 生命周期](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-actor-lifecycle)

用户已确认 Actor 与 Component 都支持 Begin/Tick/End。生命周期增量沿已有 TypeId 行为绑定扩展适用对象，落实以下完整顺序：

- Begin/Tick：按 World 的 Actor 顺序，每个 Actor 先执行自身，再按组成顺序执行组件。
- End：Actor 顺序反向；每个 Actor 先结束组件，按组成顺序反向执行，最后结束 Actor 自身。
- 运行中增加组件，只对新组件 Begin；移除组件只 End 该组件，Actor 的运行周期继续。
- 禁用 Actor 暂停自身与组件 Tick；禁用单个组件只暂停该组件。二者都不重新配对 Begin/End，相关任务取消策略保持。
- 只有成功完成 Begin 的对象参与对应 End；Actor/组件 Begin 失败，结束已成功开始的前缀。Actor Tick 失败与组件 Tick 失败采用同一 World 停止及清理合同。

组件的数据准备须在 Actor Begin 之前完成，Actor Begin 因而能访问有效组成；如依赖组件的游戏 Begin 副作用，应遵守上述顺序。当前以默认构造、Detached 组成及注册验证承担初始化边界，没有新增空的 Initialize/DataReady 钩子。文件加载就绪通知仍属于后续序列化设计。

## 文件序列化保存什么

归档消费现有类型系统，普通声明字段由统一机制读写；扩展同一字段定义表达持久化用途，与 Hidden 的公共可见性分开。文件使用版本、稳定类型/字段标识和文档内对象键；Runtime 的 TypeId/ObjectId/FieldId、指针和内存偏移不落盘。

| 数据 | 处理方式 |
| --- | --- |
| Actor/Component 具体类型、文档内身份 | 保存，并在加载时映射到新 Runtime 身份 |
| Actor 顺序、组件组成顺序、显式根与 parent | 保存权威关系，owner 等反向关系按同一 World 规则重建 |
| 局部 TRS、Material、声明为持久化的业务字段 | 沿类型描述保存；世界矩阵从空间树重新计算 |
| 名称、Actor/Component enabled | 补齐其明确的数据归属与类型接线，不能因当前未反射而遗漏 |
| MeshRef | 使用明确的资源编码或资源引用；共享资源关系应保留，不能保存 shared_ptr 地址 |
| member_world、begun、任务、Tick/呈现目录、GPU 状态 | 加载后由所属模块重建，不作为对象内容恢复 |

文档根和允许保存的引用范围必须明确。GC 可达性不直接决定保存范围；弱引用不扩大保存集合，文档外引用通过明确资源解析规则处理，不能静默丢失。首片覆盖当前编辑器实际拥有的数据与网格，格式版本不支持时明确报错。

## 加载流程与失败边界

1. 校验文件版本、类型和字段，宿主先注册本场景需要的类型；遵守 Runtime 首次创建对象后封闭注册的现有合同。
2. 创建受普通根保护、尚未对文档公开的新对象，建立文档键到 ObjectId 的映射。
3. 通过类型驱动的解码填充新对象数据，解析共享资源及强弱引用。
4. 组装最终 Actor/Component 图，验证同 Actor 空间树、唯一根、组成顺序、类型与 TRS；复用正常创建所用的 World 不变量。
5. 完成数据就绪处理与 World 注册；全部成功后交给文档，默认保持停止运行。替换当前场景只能发生在新场景可用之后。
6. 加载失败清理本次创建的完整新图和准备资源，包括组成引用环，当前场景保持可用。数据就绪回调的外部副作用不属于可回滚文件数据，回调能力必须在实现前约束清楚。

这条路径恢复到新图，因此通用的存活对象原位交换与撤销历史不是首片前置条件。不同 Runtime 中重新加载会得到新的运行时身份；文件键承担跨加载关联。

Runtime 负责人已确认此范围没有必须先引入交换或撤销机制的障碍；现有 create、ObjectRoot、类型和引用机制可作为新图构建基础。create 已产生 Runtime 身份，“尚未公开”由加载器与 World 保证，不代表另设一套临时对象身份。

保存首片在停止运行的 World 的宿主边界捕获一致数据，再编码写文件；正式文件只在新文件完整写出后替换。文件加载不恢复旧任务，也不隐式启动游戏。

## 职责与端到端终点

- Runtime/反射：完整字段描述、持久化选择、通用值与引用编码/解码、对象构造与加载就绪合同。
- World：对象组成与世界注册的清晰入口、有序运行生命周期、关系与空间校验。
- geometry/资源：现有 Mesh 的文件表达与共享资源恢复。
- 编辑器：保存/打开入口及新场景发布，当前阶段不迁移撤销实现。

验收以真实场景贯通：创建 Actor 与根 StaticMeshComponent、添加空间子组件、修改名称/启用/TRS/材质、保存，在新上下文中打开后保持内容和引用关系；start→Tick→stop 顺序可预测；再次 start 正确配对；退出与销毁清理任务；坏文件、坏引用或非法树不替换当前场景。开发与发布配置使用相同有效断言，只补必要端到端场景。
