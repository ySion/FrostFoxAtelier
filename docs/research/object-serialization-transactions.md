# 统一对象归档、编辑事务与生命周期

范围更新（2026-10-10）：用户决定暂缓撤销事务，优先实现 Actor/Component 生命周期与文件序列化。生命周期和 Scene v2 已落地，当前合同见 [World](../modules/world/README.md) 与[项目场景](../modules/world/project-scenes.md)。通用对象图归档、业务引用和容器编码仍未完成；下文保留为后续设计参考，原位恢复、值交换和历史迁移不是当前功能的前置工作。

状态：2026-10-02 设计提案；World 与 Runtime 已完成首轮协调，方向一致，具体接口尚未冻结或实现。用户要求撤销重做复用反序列化，以类似 UObject 的共同对象基础承载业务对象，并参考 UE 生命周期。本记录不改变已验收代码的当前合同。

## 当前实现

- `atelier_reflect` 使用 Clang LibTooling 生成 `NativeRegistration` 调用，沿 `TypeDefinition` 发布到 Runtime；语言类型定义进入同一体系。没有独立的序列化类型表，也尚无通用归档读写模块。[自动反射](../reflection.md)
- `core.Object` 已是 Runtime 对象的共同语义根，原生对象不要求统一 C++ 头部。值类型与对象类型分别表达内容和身份。[对象继承](../runtime-model.md#正式对象单继承)
- 编辑器 `Session::Change` 当前按 Actor、Component、TRS、名称分别记录与回放；删除记录持有普通对象根，恢复同一 ObjectId。[Session](../../src/editor/session.hpp)
- `Hidden` 控制公共字段目录，不是持久化策略。World 的 components、owner、root、parent 是隐藏引用；enabled、网格资源和编辑器名称也需要明确接入。不能把当前公共字段列表当成完整存档字段表。[World 声明](../../src/world/world.hpp)

## 建议统一的合同

### 共同对象基础

具有独立身份的业务对象使用现有 `core.Object` 类型根，包括 Actor、Component，以及后续真正需要身份的文档或资产对象。Vec3、Quaternion、String、Array 等保留值语义。是否需要可直接继承的 C++ Object 外观由实际调用需要决定；权威类型、身份、引用和生命周期继续由 Runtime 管理。

### 同一状态编码与恢复机制

文件存取和编辑事务共同消费现有类型定义中的字段、值与引用描述。字段可见性、持久化、事务参与和派生临时状态需要独立表达，避免用 Hidden 推导保存策略。生成器与语言定义生产者遵守同一合同；普通业务类不重复编写逐字段读写逻辑。

文件记录跨运行所需的类型、字段和对象标识；内存事务可以使用当前上下文的身份并持有必要对象根。两种用途共享状态遍历与恢复，不要求使用相同字节格式。文件版本必须明确；首片不承诺任意历史布局迁移。

Runtime 建议首片显式声明哪些字段参与持久化或事务。隐藏字段名目前仍保存在内部 `member_names`，缺少的是与完整字段描述、策略和受保护访问的关联；补齐现有描述即可，不需要重新发现 C++ 字段。

### 撤销与身份

- 属性编辑捕获受影响对象或字段的前后状态；撤销与重做原位恢复，不重新构造仍然存活的对象。拖动预览继续合并为一次事务。
- 增删与关系编辑记录对象存在于文档中的状态、顺序和引用关系；事务持有恢复所必需的对象，历史裁剪释放这些持有关系。
- 可撤销删除不等于 Runtime 显式销毁。已经终结的对象代际不能通过反序列化复活；遇到外部销毁必须明确拒绝或使相关历史失效。
- World 关系通过统一的校验与提交入口恢复，维持同 Actor 空间树、唯一显式根、组件顺序等不变量。对象图遍历范围不能直接等同于 GC 可达集合。
- 解析、值准备、引用解析和关系验证在发布前完成；恢复失败不暴露半完成场景。需要 Runtime 确认受支持值类型的无失败提交机制，不能依靠可能再次失败的普通赋值来保证回滚。

Runtime 已确认 `assign_value` 的现有失败保证不足以直接承载事务。建议扩展既有值操作：先准备临时拥有值和引用变更，再以无分配、无抛出、无用户通知的替换操作提交。首片不能提供此保证的原生字段应明确拒绝参与恢复。恢复期间禁止存储借用；对象地址与 ObjectId 保持，容器内部缓冲允许替换。

提交后的通知失败按“已提交、通知失败”报告，不回滚已经提交的数据，也不承诺回滚通知产生的外部副作用。历史同时受条数和字节预算约束。弱引用不会仅因被编码进历史而转为强引用。

关系归档只保存权威组成、根、父关系和文档成员顺序；owner 等反向关系由同一 World 校验与提交机制重建，避免互相矛盾的双份事实。

## UE 参考与本项目建议

UE 的 `Modify` 在事务记录期间保存对象状态；`FTransaction` 表达一组序列化的可撤销对象变化，并提供对象、数组和属性快照入口。这支持以对象状态归档作为通用编辑事务基础，但不能据此推导所有撤销都必须保存整个场景。[Modify](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UObject/Modify)、[FTransaction](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/FTransaction)

UE 在 UObject 上区分构造后的 `PostInitProperties`、加载后的 `PostLoad`、事务应用后的 `PostEditUndo`。Actor 另有 BeginPlay／EndPlay；组件的注册负责建立渲染等场景参与状态。[UObject](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UObject)、[Actor 生命周期](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-actor-lifecycle)、[组件注册](https://dev.epicgames.com/documentation/en-us/unreal-engine/components-in-unreal-engine)

本项目建议按职责保留以下阶段；名称与具体接口待 Runtime 协调后确定：

| 阶段 | 职责与触发边界 |
| --- | --- |
| 构造完成 | 建立新实例的默认状态；原位事务恢复不重复构造 |
| 加载完成 | 值与引用解析、整体关系验证成功后，恢复派生状态 |
| 事务恢复完成 | 原位恢复完成后通知受影响对象与消费者；不暴露中间字段状态 |
| 注册／反注册 | 维护 World 成员关系、查询索引和渲染参与状态 |
| Begin／End | 按 World 运行参与周期配对；enabled 只暂停 Tick 并取消相关任务 |
| 销毁 | 遵守现有 Runtime 身份终结、GC 与原生析构合同；不代替业务 End |

第一片沿用停止 World 后编辑的规则。加载与普通属性撤销不触发 Begin／End；以后如开放运行中结构编辑，真实加入或离开运行中的 World 仍须触发生命周期。GPU 资源沿既有退休机制释放，不因参考 UE 就新增通用异步对象销毁系统。

## 协调职责与验收终点

- Runtime：类型元数据、受保护的状态捕获／恢复、值与引用提交、对象身份和通知合同。
- World：场景成员与空间关系的验证、恢复提交、派生索引与呈现刷新。
- 编辑器：事务范围、预览合并、历史游标与操作入口，迁移后删除当前按业务种类编写的重复状态回放。

先以现有真实场景贯通：修改任意已支持反射字段、撤销／重做；组件增删与层级恢复保持身份和顺序；保存后在新 Runtime 中重开并恢复引用；失败输入保持原场景；历史裁剪不泄漏对象；回调观察不到半恢复状态。只使用必要的端到端验收。

## 首轮协调结果与待落实项

Runtime 负责人同意复用 `core.Object`、现有注册链及普通 GC 根，补齐完整字段描述和安全状态恢复，迁移编辑器的专用回放分支。本阶段无需为了序列化改变所有原生对象布局。

实现前还须明确：恢复准入与 GC 阶段的衔接、批次提交接口、恢复通知适配，以及名称、enabled、MeshRef 的状态表达。Runtime 建议先以无外部资源的对象图验收底层恢复；完整场景保存交付仍须覆盖当前可编辑数据与可绘制资源，不能悄悄遗漏名称、启用状态或网格。
