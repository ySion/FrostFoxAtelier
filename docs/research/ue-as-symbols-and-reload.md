# UnrealEngine-Angelscript：符号可见性、内部模块与热重载事实核查

调查日期：2026-09-20。本文记录第一方资料与源码能够支持的事实，不据此替 FrostFoxAtelier 确定语言或运行时契约。

## 证据范围

- 核心源码：[Hazelight/UnrealEngine-Angelscript，固定提交 `f347f57f05e992976b6c2be6612303b7e8099502`](https://github.com/Hazelight/UnrealEngine-Angelscript/tree/f347f57f05e992976b6c2be6612303b7e8099502)，提交日期 2026-09-11。本次使用本机已有 GitHub 授权正常读取官方仓库；这些链接需要相应 Unreal Engine 源码权限。
- 公开编辑器工具源码：[Hazelight/vscode-unreal-angelscript，固定提交 `ffc7ab2948589abb04200b7dcedd326eaeaf7a4d`](https://github.com/Hazelight/vscode-unreal-angelscript/tree/ffc7ab2948589abb04200b7dcedd326eaeaf7a4d)。LSP 仅用于交叉核对编辑体验，不能代替 VM 或编译器的行为证据。
- 官方文档：[Unreal Engine Angelscript](https://angelscript.hazelight.se/)，及其[固定文档提交 `3864c72fa3c1ef67413dfaad24417bad01879b4d`](https://github.com/Hazelight/Docs-UnrealEngine-Angelscript/tree/3864c72fa3c1ef67413dfaad24417bad01879b4d)。官方安装文档说明了[核心仓库需要 Unreal 源码访问权限](https://angelscript.hazelight.se/getting-started/installation/)。未登录网页返回 404 不能证明该仓库或源码不存在。

本次为源码静态核查，没有构建并运行 UE-AS 示例；“高置信度”表示固定提交中的直接实现证据，不表示已经在目标游戏工程完成动态验收。核心提交来自调查时的默认分支 `angelscript-master`，不将它的行为自动推广到所有已发行引擎版本。

用户确认：这轮先核实事实，再确定 FrostFoxAtelier 的布局更新预期。本文不改变项目运行时、命名空间、模块或热重载契约。

## 主张、证据与置信度

| 待核查主张 | 结论 | 直接证据 | 置信度 |
|---|---|---|---|
| 普通跨文件引用必须手写 import 才能看到其他脚本类型和命名空间函数 | 不需要。该 fork 直接查同一脚本引擎中的全局符号表；显式函数 import 的注册已经直接返回 | [类型查找](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L6491-L6525)、[import 与函数查找](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L5593-L5645) | 高 |
| 所有文件的所有符号都无条件可用 | 不成立。根命名空间全局函数默认不对其他文件开放；`local` 函数也会被跨模块查找过滤 | [默认设置](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Public/AngelscriptSettings.h#L101-L103)、[查找过滤](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L5620-L5635) | 高 |
| UE-AS 底层没有模块 | 不成立。预处理按文件建立模块描述，编译时实际创建 `asCModule`；引擎保有活动模块表 | [文件到模块](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Preprocessor/AngelscriptPreprocessor.cpp#L89-L109)、[创建 VM 模块](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L2932-L2945)、[引擎符号表与模块表](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_scriptengine.h#L377-L394) | 高 |
| 模块就是命名空间 | 不成立。模块身份由文件路径生成，namespace 是另一项声明/查找属性；两者在源码中独立保存 | [模块名转换](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Preprocessor/AngelscriptPreprocessor.cpp#L89-L104)、[类的 namespace](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Preprocessor/AngelscriptPreprocessor.cpp#L729-L757)、[按 namespace 查函数](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L5620-L5635) | 高 |
| 不写 import 就不需要记录依赖 | 不成立。编译器记录跨模块依赖，区分结构依赖、硬值依赖，并记录首次引用的位置 | [依赖结构](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_module.h#L251-L262)、[记录依赖](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L6735-L6816) | 高 |
| 模块对业务编写完全不可观察 | “通常无需管理导入”成立，“完全不可观察”过强。除文件局部函数外，源码还提供按模块名模式限制使用的预处理指令 | [限制指令解析](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Preprocessor/AngelscriptPreprocessor.cpp#L3211-L3238)、[限制检查](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L3227-L3277) | 高 |
| 重载意味着新函数直接覆盖旧同名符号 | 不成立。普通声明收集同名候选并检查签名；同一可见候选集合中重复的普通函数签名会报错。热重载是另一条版本更新流程 | [候选与重复检查](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L5114-L5177) | 高 |

## 源码证据与解释

### 1. 业务代码无需承担普通跨文件导入管理，但可见性有规则

`asCScriptEngine` 同时持有 `allScriptGlobalFunctions`、`allScriptDeclaredTypes`、`allScriptGlobalVariables` 三类全引擎符号表。`asCBuilder::GetType()` 在已注册原生类型之外，直接遍历 `engine->allScriptDeclaredTypes`，按类型名与 namespace 匹配；此处没有要求调用方提交一份 import 清单。[引擎表](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_scriptengine.h#L381-L394)、[类型查找实现](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L6491-L6525)。

`asCBuilder::RegisterImportedFunction()` 以 automatic imports 为理由直接返回。`GetFunctionDescriptions()` 则从全引擎脚本函数表收集同名、同 namespace 的候选，并添加已注册的原生全局函数。不过来自其他模块的候选会经过两个过滤条件：根命名空间函数受 `bExposeGlobalFunctionsToOtherScriptFiles` 控制，`local` 函数不可跨模块使用。该设置的默认值是 `false`。[注册与查找](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L5593-L5645)、[默认配置及其注释](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Public/AngelscriptSettings.h#L101-L103)、[`local` 的解析](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L4586-L4613)。

因此，准确表述是：**已经纳入同一脚本引擎的定义通过共同符号空间查找，业务代码通常不需要 import；实际可用性仍由 namespace、文件局部性、配置和访问规则决定。** 不能推广成“任何存在的符号在任何位置都能直接使用”。全局变量还有自身的 namespace / `bAllowAnyModule` 查找分支，本次没有把函数的规则直接套到变量上。[全局变量查找](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L1231-L1280)。

公开 LSP 的 `server.ts:215` 同样明确注明不再支持非自动导入；这与核心编译器相符，但它只是辅助证据。[LSP 设置读取](https://github.com/Hazelight/vscode-unreal-angelscript/blob/ffc7ab2948589abb04200b7dcedd326eaeaf7a4d/language-server/src/server.ts#L210-L215)。

### 2. namespace 和 module 是两个独立维度

预处理器 `AddFile()` 为文件新建 `FAngelscriptModuleDesc`。`FilenameToModuleName()` 将相对路径中的 `.as` 去掉，再把 `/` 换成 `.`。按该实现推导，`Gameplay/Ships/Ship.as` 的内部模块名是 `Gameplay.Ships.Ship`，无论文件里声明了哪个 namespace。[文件建模](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Preprocessor/AngelscriptPreprocessor.cpp#L89-L109)。

类描述的 `Namespace` 从语法块单独赋值，并加入所在文件的模块；函数查找则同时使用 namespace 与所属 module。它们不是同一个键，也不是同一种职责。[类描述](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Preprocessor/AngelscriptPreprocessor.cpp#L729-L757)、[函数查找](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L5620-L5635)。LSP 也把 namespace 的多个声明归属于多个 `declaredModule`，并据此查依赖，进一步印证二者没有一一对应关系。[LSP namespace 依赖](https://github.com/Hazelight/vscode-unreal-angelscript/blob/ffc7ab2948589abb04200b7dcedd326eaeaf7a4d/language-server/src/as_parser.ts#L1335-L1348)。

### 3. VM 内部保留真实模块及分阶段编译

`FAngelscriptManager::CompileModule_Types_Stage1()` 使用模块名调用 `Engine->GetModule(..., asGM_ALWAYS_CREATE)`，得到真实的 `asCModule`；随后把代码段交给该模块。非初始编译会先使用临时名称，是否采用新模块由后续流程处理。[模块创建](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L2932-L2945)、[添加代码段](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L3003-L3015)。

编译管理器先为本批次各模块准备编译状态、并行解析，再为模块生成类型；之后还有函数生成与代码编译阶段。它并非必须把整个工程拼成一个 VM 模块，才能提供无需手写前置声明和导入的编辑体验。[类型阶段](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L1893-L1955)、[函数与代码阶段](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L3018-L3056)。这里证明了分阶段实现，不代表所有类型布局递归或初始化依赖都合法。

`asCModule` 保存自己的类型、全局变量与 `moduleDependencies`。依赖信息区分 `bIsStructuralDependency`、`bIsHardValueDependency`，并记录首次引用行列；编译器通过 `MarkDependency()`、`MarkHardValueDependency()` 等入口填充它。[模块状态](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_module.h#L244-L269)、[依赖记录](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L6735-L6816)。

“透明”适合描述普通调用方不用手动导入，也不用在每次调用中写模块定位；不适合描述模块在 VM 中消失。源码显示，全引擎查找与模块级编译、依赖、所有权可以同时存在。

### 4. 存在可选的模块使用限制

预处理器识别 `#restrict usage allow` 与 `#restrict usage disallow`，把模式保存在模块描述上。编辑器路径中的 `CheckUsageRestrictions()` 遍历实际编译依赖，将使用方的模块名与模式做 wildcard 匹配，违规时生成编译诊断。[预处理](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/Preprocessor/AngelscriptPreprocessor.cpp#L3211-L3238)、[检查](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L3193-L3279)。

这说明不强制 import 并不妨碍依赖约束；不过该机制的存在不等于要求所有项目使用，也不能表述为运行时沙箱或安全隔离。

### 5. 函数重载与热重载必须分开核查

普通函数声明先按名称和 namespace 取得已有候选，再比较参数类型、传递修饰和相关方法属性。普通函数同一可见集合中的重复签名会得到“函数已经存在”的错误，返回类型不同也不能自动当成普通重载。它不是按名字把旧函数覆盖掉。[重复签名检查](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_builder.cpp#L5114-L5177)。

多个签名形成重载候选集合、继承中的方法覆盖、重新编译后替换执行版本，是三件不同的事。能否替换一个正在使用的函数、是否需要更新调用点、何时释放旧版本，必须依据热重载流程另行回答，不能从全局符号表的存在推出。

## 热重载：更新引用、替换模块与迁移实例

### 6. 替换执行版本包含依赖分析和引用修补

管理器为重新编译的模块关联新旧版本，收集类型映射并检查结构差异。对于依赖者，只有代码变化时也并非一律不重编译：结构依赖或硬值依赖仍会触发重编译，其他合适的依赖可以仅更新引用；结构变化则触发依赖者重编译。[新旧映射与差异](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L1986-L2039)、[依赖处理](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L2090-L2125)。

`asCModule::DiffForReferenceUpdate()` 在匹配新旧全局函数时使用名称、namespace 与函数签名，把旧函数到新函数的关系放进更新表。`UpdateReferencesInScriptBytecode()` 进一步处理函数 ID、类型 ID、全局变量地址和包含类型信息的字段偏移引用。它不是仅在一个按名字查找的表中覆盖 bytecode 指针。[函数映射](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_module.cpp#L2260-L2287)、[字节码引用更新](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/ThirdParty/source/as_module.cpp#L2558-L2705)。

`SwapInModules()` 将旧 VM 模块标为过期并改名，将新模块改为正式名称，更新 `ActiveModules`，再重建活动类型索引。成功路径之后调用 `DiscardModule()` 与 `DeleteDiscardedModules()` 清理旧模块。失败路径还存在反向映射，以撤销对其他模块引用的更新。[模块替换](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L1651-L1706)、[清理与反向更新](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L2698-L2765)。这些操作不能单独证明任意用户初始化副作用都有事务性回滚。

UE 侧的 soft reload 会把已有 `UASFunction` 的 `ScriptFunction` 指向新脚本函数，并更新参数相关类型信息。[函数重绑定](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/AngelscriptClassGenerator.cpp#L4152-L4167)。

### 7. soft reload 包含受限布局更新，不只是方法体替换

这里需要同时纠正两个过强表述：“任意布局都能实时更新”和“soft reload 完全不能改变布局”都不符合该提交的实现。

生成脚本类时，在启用热重载且满足编辑器/开发构建条件的路径中，类实例存储额外预留 128 字节。新脚本类型大小若超过旧 `UClass` 已有 `PropertiesSize`，会被判定为必须 full reload。这是明确的容量条件，不能将 128 字节理解成每次重载都能无限追加的预算。[预留空间](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/AngelscriptClassGenerator.cpp#L3577-L3588)、[大小检查](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/AngelscriptClassGenerator.cpp#L1304-L1315)。

`DoSoftReload()` 的实现包括：

1. 枚举对应 `UClass` 的实例，重新链接已有反射字段的偏移，刷新相关序列化与引用收集数据。
2. 收集旧、新脚本属性，可展开嵌套结构，按属性路径名称和相同类型匹配可复制状态，记录旧/新偏移。
3. 按实例相对 CDO/defaults 的差异选择需保存的值，放入临时缓冲区。
4. 析构旧脚本实例，在同一存储位置重新初始化，再按新偏移恢复选中的兼容状态。

因此，它确实做了实例迁移工作，而且与 UObject/反射深度协作。不是所有值无条件逐字节保留，也不是只改元数据即可让已有对象自动适应新布局。[字段重链接](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/AngelscriptClassGenerator.cpp#L4036-L4104)、[属性匹配和缓冲区](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/AngelscriptClassGenerator.cpp#L4373-L4475)、[状态保存与原位重建](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/AngelscriptClassGenerator.cpp#L4498-L4567)。

### 8. full reload 有明确触发条件和执行时机

类生成器会对更换基类、已记录属性的删除/类型或定义变化、特定新增属性、struct 字段偏移变化、新增 BlueprintEvent 等情况提高重载要求。新增某些普通反射成员可能只是 `FullReloadSuggested`；新增 struct 字段、要求反射属性的字段以及新布局超出已有容量等会要求 full reload。不能用“新增字段”一个标签推导所有情况的结果。[属性与继承检查](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/AngelscriptClassGenerator.cpp#L1083-L1200)、[新方法和大小检查](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/ClassGenerator/AngelscriptClassGenerator.cpp#L1270-L1315)。

主更新路径在非编辑器环境或存在游戏世界时只请求 `SoftReloadOnly`，否则允许 full reload。对 `FullReloadSuggested`，PIE 中可先执行 soft reload，但新反射成员的 UE 暴露可能等到 PIE 结束后的 full reload 才更新；对 `FullReloadRequired`，若当前只能 soft reload，则保留旧代码并安排后续完整重载。[执行时机](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L1574-L1583)、[三种处理分支](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L2624-L2684)、[后续排队](https://github.com/Hazelight/UnrealEngine-Angelscript/blob/f347f57f05e992976b6c2be6612303b7e8099502/Engine/Plugins/Angelscript/Source/AngelscriptCode/Private/AngelscriptManager.cpp#L2853-L2872)。

官网的概述区分了编辑器无需重启与 PIE 中非结构修改无需结束游戏；当前源码展示了 soft reload 对某些受限布局变化更细的处理方式。两者都不支持“任何布局变化在游戏运行中都能无条件生效”的说法。[官网热重载说明](https://angelscript.hazelight.se/)。使用预编译脚本缓存时，官方文档另行明确关闭脚本文件加载和热重载，不能把开发模式能力推广到该运行方式。[预编译模式](https://angelscript.hazelight.se/cpp-bindings/precompiled-data/)。

## QA：源码判定矩阵

以下是针对主张的静态问答，不是已执行的 UE 动态测试记录。

| 问题/场景 | 本轮能够支持的答案 |
|---|---|
| A 文件使用 B 文件的公开命名空间函数，需要显式 import 吗？ | 普通路径不需要；全引擎查找仍应用可见性/使用限制。 |
| 任意根命名空间全局函数能跨文件调用吗？ | 默认设置下不可以；有相关暴露配置。 |
| 一个 namespace 能覆盖多个文件的声明吗？ | 可以；namespace 与由文件路径生成的模块身份分别保存。 |
| 一个新定义与旧定义同名同签名，会自动顶掉旧定义吗？ | 普通重复声明会报错，版本替换属于热重载流程。 |
| 更新函数实现时，只替换名字表就足够吗？ | 现有实现还做依赖分析、引用更新和可能的依赖者重编译。 |
| soft reload 能重新安排某些对象字段吗？ | 实现存在字段偏移重绑定、兼容状态匹配与原位实例重建，但受分析器及容量条件限制。 |
| 新对象布局超过旧实例可用大小怎么办？ | 必须 full reload；仅允许 soft reload 时保留旧执行版本。 |
| struct 字段偏移变化能按 UObject 的枚举方式迁移吗？ | 对相关属性偏移变化要求 full reload；源码明确指出不能遍历所有 struct 实例来修改。 |
| 新反射成员立即在 UE 中完全可见吗？ | 不保证；部分变更在 soft reload 后仍等待 full reload 才完成反射暴露。 |
| 当前函数正在执行或挂起，能把调用栈也迁移到新布局吗？ | 本轮没有完整核实活动上下文、任务和原生回调的全部处理，不作承诺。 |
| 发布版本使用预编译脚本缓存，也能同样热更吗？ | 官方描述的缓存使用路径关闭热重载；开发模式另行处理。 |

## 可以确定的事实与仍待决定的项目问题

对该固定提交，最准确的概括是：**业务侧通常不管理显式跨文件导入；编译器通过共享符号空间解析引用，并在内部保留按文件组织的 VM 模块、依赖和执行版本。热重载协调模块替换、引用修补与受限实例迁移。**

“业务代码是否必须声明模块/import”“名称如何组织与访问”“编译/缓存怎样切分”“一批更新怎样发布”是可以分别设计的问题。源码证明了这些职责能够以一种具体方式组合，不证明 FrostFoxAtelier 必须沿用全部实现，也不证明 namespace 应成为内存或重载所有权单位。

上一轮对 FrostFoxAtelier 提出的“显式导入、模块依赖初版无环”仅是未经选择的方案；本轮用户要求先核实事实，因此不将它升级为项目规范。后续如果要定契约，优先回答：公开符号集合由什么决定、名称冲突和访问控制如何处理、哪些更新需要一起提交，以及布局变化允许保存哪些实例状态。

## 尚未验证的事项

- 没有运行两文件最小工程来动态验证各类符号的完整可见性矩阵；上面的函数默认值与过滤条件来自固定提交源码，具体游戏可以覆盖配置。
- 没有穷尽所有全局变量、模板、access specifier、编辑器专用定义与原生绑定的限制，不把“自动导入”写成“无限制访问”。
- 没有据此断言任意循环依赖都合法。名称/函数相互引用、值布局递归、继承环和全局初始化环需要分别检查。
- 本轮已核查上述 UObject soft reload 迁移路径，但未穷尽活动栈帧、异步任务、委托、原生缓存地址、所有模板实例或 JIT 调用点更新。也没有实测更新耗时、暂停时间和运行中状态保留的完整矩阵。
- 对 FrostFoxAtelier 的 namespace、文件、编译批次、重载发布边界尚未作架构决定。这里只确认 UE-AS 证明了一种可行组合：业务侧弱化显式导入管理，内部仍保留真实编译模块、依赖与可见性规则。
